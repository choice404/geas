# The Geas ABI

This is the normative ABI for Geas. It covers everything that crosses the boundary between a compiled module, the intermediary runtime, and a foreign host: the value representation, the thunk frame every pledge is called through, the descriptor tables a module publishes, the registrar protocol, the symbol mangling, the shape hash, and who owns what. Two headers are the source of truth for every layout and every call named here: `runtime/include/geas/geas_abi.h` holds the value representation, the status and state enums, and every descriptor table a module emits, and `runtime/include/geas/geas.h` includes it and adds the host API with the structs that only a host fills or reads, `GeasRuntimeConfig`, `GeasEvent`, `GeasAttrInfo`, `GeasCourseInfo`, `GeasInameEntry`, and `GeasStoreTerm`. A change to the wire lands in those headers first and this document moves with them. What the two headers promise from 1.0.0 on is the subject of [Stability](#stability), the last section.

The Python package in `interop/python/geas/` implements this ABI through
`ctypes`, with the host surface exported from `geas`.

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
        GeasString s;    /* String or Bytes */
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
    GEAS_TY_PLEDGE_REF, GEAS_TY_SUM, GEAS_TY_INSTANCE, GEAS_TY_BYTES
} GeasTypeTag;
```

`GEAS_TY_INSTANCE` is 16 and stays internal. `GEAS_TY_BYTES` is 17, appended after it, with every earlier tag unchanged. Bytes rides `as.s.ptr` and `as.s.len`, arbitrary bytes including zeros, no terminator, and `tag` zero. Its instance ownership and deep copy follow String, and equality checks length then `memcmp`. No public struct grows.

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

An Option or Result payload rides in `as.box` as a pointer to a single `GeasValue` the instance owns. `None` carries a null box and nothing else, and a value whose type tag is not one of the three sum shaped ones carries a `tag` of 0. An `Ok` or `Err` of Unit may ride with a null box, the spelling a body answers it in, and reads the same as a box holding Unit.

A list carries its elements as a contiguous `GeasValue` array behind `as.list.data`, `len` of them live, `cap` allocated, `elem_ty` the declared element tag. A tuple rides the same union arm: the data pointer is the `GeasValue` array, `len` is the arity, `cap` equals `len`, and `elem_ty` is 0 because a tuple's elements need not agree.

A record rides the list arm too: `GEAS_TY_RECORD`, the data pointer an `GeasValue` array holding the fields in declaration order, `len` the field count, `cap` equal to `len`, `elem_ty` 0. A sum variant's payload is the same shape, `GEAS_TY_SUM` with the variant's fields in declaration order behind the data pointer; a variant with no payload carries an empty arm, data NULL and `len` 0. Field access in compiled code is a slot read by declaration index, so the order is normative.

**The map representation.** A map, `GEAS_TY_MAP`, rides the list arm as well, and this layout is canonical: the data pointer holds interleaved key, value pairs in insertion order, the key of pair i at slot 2i and its value at slot 2i+1, `len` counts slots so it is always twice the pair count, `cap` is allocated slots, and `elem_ty` is the key's tag alone, one of the keyable scalars `Int`, `UInt`, `Bool`, `Byte`, `Char`, `String`. A map holds each key once. The value type rides no field of the map: the contract's type table names it, and the checked boundary holds every value in the map to it. Insertion order is normative: it is the order any serialization sees, the order equality compares, and updating an existing key replaces its value without moving its pair. Lookup and insert scan the keys linearly through `geas_value_eq`, O(n) in the pair count, the v1 tradeoff that keeps the representation one union arm deep.

**The internal instance tag.** `GEAS_TY_INSTANCE`, the tag after `GEAS_TY_SUM`, is internal to compiled modules and never crosses the ABI. It is the signed instance handle a cross-contract `Contract.sign(...)` returns inside a pledge body: `as.box` holds the `GeasContract*`. The language already rejects a contract type at every boundary position, pledge parameters and returns, vows, record and variant fields, and provisional clause signatures, so no thunk frame, vow storage, or descriptor value ever carries this tag, and a foreign host never meets it. `GEAS_TY_PLEDGE_REF`, the tag before `GEAS_TY_SUM`, keeps its number and carries nothing: pledge values are not part of this version of the language, the checker refuses a pledge type and a bare `instance.pledge_name` wherever either appears, and no compiled code builds a value with this tag. Neither tag is a value a boundary carries, and the checked boundary, the registrar, and the wire codec all refuse both. Its helper arms are the reference-handle semantics the language pins: `geas_value_deep_copy` copies the handle, not the contract, the one deliberate exception to value semantics, and `geas_value_eq` is handle identity. The debug renderer spells it `<instance>`.

The runtime exposes deep value helpers to hosts and thunks alike: `geas_list_new`, `geas_list_push`, `geas_list_get`, `geas_list_set`, `geas_map_new`, `geas_map_set`, `geas_map_get`, `geas_tuple_new`, `geas_value_eq`, and `geas_value_deep_copy`, which recursively copies any supported value into instance owned memory, string bytes, list, map, tuple, record, and sum elements, and boxed payloads included. After a deep copy, nothing in the destination aliases memory the instance does not own. `geas_value_eq` is the structural equality the language's `==` lowers onto for the deep shapes: type tags first, the sum shaped `tag` next, then elements recursively. Two maps compare pair by pair in insertion order, keys and values both, so the same pairs written in another order read unequal, which keeps the answer deterministic and matches what serialization sees. `geas_value_eq` follows a value down at most 4096 levels, `geas_value_deep_copy`'s own line, and a pair it would have to follow further, a cyclic value compared with itself among them, reads unequal; a NULL operand, a tag mismatch, `GEAS_TY_PLEDGE_REF`, and a tag past the enum read unequal too. `geas_value_deep_copy` refuses a value deeper than 4096 levels and one that claims a length with no data behind it or a list or a map longer than its capacity with `GEAS_ERR_TYPE`, and a value of more than `GEAS_VALUE_MAX_NODES` nodes, a value that shares its substructure many times over counted as the tree it unfolds to, and an element count no allocation could hold with `GEAS_ERR_OOM`. The two growing helpers stop at the same budget: `geas_list_push` answers `GEAS_ERR_OOM` once the list already holds `GEAS_VALUE_MAX_NODES - 1` elements, and `geas_map_set` once a new pair would take the map past it, the container counting as one node of its own, see The node budget.

`geas_string_eq(a, b)` answers 1 when two string values hold the same bytes and 0 otherwise; compiled code lowers `==` on two `String` values onto it.

The value and allocation helpers cross the boundary with these shapes, every allocating one hanging its memory off the instance it was handed:

```c
uint8_t*  geas_bytes(GeasContract* c, uint64_t n);
GeasValue* geas_box(GeasContract* c);
GeasValue  geas_string_copy(GeasContract* c, const uint8_t* utf8, uint64_t len);
GeasValue  geas_bytes_copy(GeasContract* c, const uint8_t* ptr, uint64_t len);
GeasStatus geas_bytes_copy_into(GeasContract* c, const uint8_t* ptr,
                                uint64_t len, GeasValue* out);
GeasStatus geas_bytes_concat(GeasContract* c, const GeasValue* a,
                             const GeasValue* b, GeasValue* out);
GeasStatus geas_bytes_from(GeasContract* c, const GeasValue* value, GeasValue* out);
GeasStatus geas_bytes_text(GeasContract* c, const GeasValue* value, GeasValue* out);
GeasStatus geas_bytes_list(GeasContract* c, const GeasValue* value, GeasValue* out);
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
| `geas_string_copy` | `geas_string_copy_into` | UTF-8 copied onto the instance |
| `geas_bytes_copy` | `geas_bytes_copy_into` | Raw bytes copied onto the instance |
| `geas_map_new` | `geas_map_new_into` | Empty map, no allocation |
| `geas_state_value` | `geas_state_value_into` | String borrowing static state bytes |

A NULL instance or `out` is `GEAS_ERR_TYPE`. The string form also refuses NULL `utf8` when `len` is nonzero, while NULL with length zero builds an empty string. Failure to allocate the string is `GEAS_ERR_OOM`, the status spelling of the direct form's NULL byte pointer; a length that cannot fit an allocation is refused with the same status. An empty map's NULL data pointer is a successful value. The map and state forms allocate nothing.

`geas_bytes_copy_into` follows the string pointer form, including its NULL and allocation checks. The raw allocator keeps the name `geas_bytes`; it returns memory, while `geas_bytes_copy` returns a tagged value. `geas_bytes_from` implements both `bytes` conversions, `geas_bytes_text` implements UTF-8 validation and `Option<String>`, and `geas_bytes_list` implements `byte_list`. Results live on the instance heap. A wrong input type is `GEAS_ERR_TYPE`, allocation failure is `GEAS_ERR_OOM`, and invalid UTF-8 is a successful `None`.

**Indexing out of bounds.** Compiled code bounds checks every list element read and write, reads through `geas_list_get` and writes through `geas_list_set`. An index outside the list, a negative one included through the unsigned cast, makes the pledge return `GEAS_ERR_TYPE` from its thunk: the status reports the fulfillment never produced a value, no latch moves, and host memory is never touched. Bytes reads use the same Int index, unsigned bounds check, and `GEAS_ERR_TYPE` fault, returning one Byte on success. This rule is normative for every compiled module.

**Indexing a map.** A map read is never a fault: compiled code lowers `m[k]` onto `geas_map_get` and wraps the answer in the `Option` the language promises, `Some` around an instance owned deep copy of the value on a hit, `None` on a miss. `m[k] = v` lowers onto `geas_map_set`, which deep copies the key and the value onto the instance before committing, replaces in place when the key exists, and appends in insertion order when it does not. `geas_map_new` builds the empty map; it allocates nothing, so it returns the value directly. `geas_map_get` hands back a borrow into the map's own pair storage, which a later `geas_map_set` on the same map may move, so a host copies it out before the next write.

**The debug renderer.** The runtime renders any supported value into its canonical debug spelling, the text a standalone executable prints for a `Main.run` error and a convenience for any host that wants a value it can log:

```c
GeasStatus geas_value_render(const GeasValue* v, char* buf, size_t cap,
                           size_t* need);
```

Bytes renders as `Bytes(00ff; 2 bytes)`, at most 32 bytes in lowercase hex, an ellipsis when more bytes follow, and the full byte count.

The spelling: `Int`, `UInt`, and `Float` as C would print them, `Bool` as `true` or `false`, `Byte` as its decimal value, `Char` as `U+0041`, `Unit` as `()`, strings quoted with `"` and `\` escaped, the bytes below 0x20 and 0x7f as `\xNN`, and every other byte, invalid UTF-8 included, as it stands, lists as `[a, b]`, tuples as `(a, b)`, records as `{a, b}` with the fields in declaration order, a sum as `#tag` with its payload in parens when it carries one, and `None`, `Some(v)`, `Ok(v)`, `Err(v)` through their boxes. A map renders as `{k: v, ...}` with its pairs in insertion order, the only order a map has, and an empty map as `{}`. Nesting deeper than eight levels renders as `...`, so a cyclic or absurd value cannot run the renderer away; the depth is the one bound, a long list renders every element, so a host sizes the buffer first. A string or list arm that claims a length with no data behind it renders as `<?>`, an Option or Result with no box behind a payload arm as `?`, `GEAS_TY_PLEDGE_REF` as `<pledge>`, and a tag past the enum as `<?>`; `Float` prints through `%g`, so the text is for reading and not for parsing back. The size protocol is `geas_iname_dump`'s: `*need` receives the full size including the terminating NUL, a `cap` at least that writes the text and returns `GEAS_OK`, anything smaller writes nothing and returns `GEAS_ERR_OOM`, so a NULL `buf` with `cap` 0 sizes the buffer.

## The thunk frame

Every pledge crosses the boundary in one shape, compiled bodies and host bound implementations alike:

```c
typedef GeasStatus (*GeasPledgeFn)(void* ctx, const GeasValue* args, size_t nargs,
                                 GeasValue* out);
```

`ctx` is the signed contract instance the fulfillment runs against. The runtime zeroes `out` before the call. A compiled thunk still validates `nargs` and each argument's type tag before touching a union arm and reports `GEAS_ERR_TYPE` on any mismatch, the line it holds for a descriptor with no type table, and writes the pledge's declared value through `out` on `GEAS_OK`. Everything a thunk allocates goes through `geas_bytes`, `geas_box`, or a helper built on them, so it is owned by the instance in `ctx` and dies with it.

`args` is the full frame, one slot per declared parameter, and every slot is an instance owned deep copy the runtime built at fulfillment entry. A pledge body never sees host memory. When the caller passed trailing parameters by reference, those slots are mutable on purpose: an implementation that updates a by-reference parameter casts away the const on its own slot and writes the new value there, and the runtime carries the final slot values back to the host at delivery.

A pledge that succeeds with an `Err` result still returns `GEAS_OK`; the status word reports whether the thunk ran, the value reports what the pledge decided. The runtime latches the outcome either way.

The frame is checked on the way in and the answer on the way out, both against the pledge's declared types all the way down, see The checked boundary. A frame that does not conform is `GEAS_ERR_TYPE` with the body never run. An answer that does not conform, which only a host bound body can write since the compiler already checked the compiled ones, is that attempt's `GEAS_ERR_TYPE`: nothing latches, the value is dropped, and the attempt is retried like any failed one when the pledge declares a `retry`. A descriptor with no type table gets the line the thunk holds and nothing below it.

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

A NULL `cfg` means all defaults. `geas_module_load` hands `so_path` to dlopen as it stands, so a relative path resolves by the loader's rules from the process working directory.

**Shutdown.** `geas_runtime_shutdown` runs in a fixed order. It raises the shutdown flag every body reads through `geas_cancel_requested` and wakes every default retry wait, stops and joins the ticker, stops the pool's intake and joins every worker once the queue drains, and only then frees every instance with its futures, store connection, heap, course, and autopark copies, then the registered descriptors and the iname table, then closes the loaded modules in the order they loaded. Nothing else may be calling into the runtime by then, and shutdown is called from a plain host thread, since it joins threads that a body, a hook, or a lock holder would be waited on by. So a call from a pledge or remedy body, an event hook, a ref `write_back` callback, the body a `together` runs, the ticker thread, or any thread holding an instance lock is refused: the call writes one line to stderr, `[libgeasrt] geas_runtime_shutdown refused: ...`, and returns with nothing raised, stopped, or freed, the runtime running exactly as before, and the host calls it again from a thread of its own. The call returns `void`, so that line is the whole answer. A host sleep callback still waiting is the host's to wake, the runtime never interrupts it.

**Freeing one instance.** A handle lives until shutdown unless the host gives it back sooner:

```c
GeasStatus geas_contract_free(GeasContract* c);
```

The instance must be over, ended by `geas_contract_break` or `geas_contract_release`, which are the two calls that reclaim its heap; an automatic break keeps the heap for the partial surface, so a host follows it with `geas_contract_break` before freeing. And nothing may still run on it: no body, retry wait, remedy pass, event delivery or queued event, course entry or join, ref `write_back` callback, `geas_course_info` or `geas_course_wait` reading its course, `geas_contract_break` or `geas_contract_release` call that has not returned yet, and no receipt still pending or held by the pool. Then the call answers `GEAS_OK`, takes the instance out of the runtime's table, and frees it with every receipt of it not yet released; from then on the handle and those receipts are gone, and any call on them is a use after free, the way `free(3)` leaves a pointer. It answers `GEAS_ERR_STATE` for a live instance, for work still in flight on it, which a later call finds drained, so a free that races a reader of the course or a write back simply waits for the next try, and when called from a body, a hook, or any thread holding an instance lock, and `GEAS_ERR_TYPE` for NULL. A runtime tick walking the instances at that moment holds it a little longer, and the last one to let it go frees it. Instances compiled code signs, resumes, or claims, inside a pledge or remedy body, a course, or a `together` body, are never freed this way: the frame keeps the handle until shutdown, and `geas_contract_free` answers `GEAS_ERR_STATE` for every one of them.

`handshake_ms` is a reserved field: the config struct's layout is pinned, so the slot stays where it is and a host leaves it zero. `GEAS_ERR_NET` is a reserved status the same way, the numbers behind it fixed forever. The cross process story lives in [docs/bridge.md](bridge.md): the compiler emits the gRPC surface, a server holds the runtime through this ABI, and nothing about serving shows through these calls.

**Loading the runtime.** Each compiled module names `libgeasrt.so.0` as a library dependency, so a host may link the runtime plainly or open it dynamically without `RTLD_GLOBAL` before calling `geas_module_load`. A host that already opened the runtime through `libgeasrt.so` with `RTLD_GLOBAL` keeps working, the symlink and SONAME identify the same library, so the module reuses that runtime without duplicate state.

**Where a module finds the runtime.** `geas build` links every module and every standalone executable with `-lgeasrt` and a `DT_RUNPATH` that starts with `$ORIGIN`, so a `libgeasrt.so.0` sitting beside the module is found first. Built from a checkout the RUNPATH carries the checkout's runtime directory too: the absolute `$GEAS_ROOT/target/geas-out` when `GEAS_ROOT` is set, and the relative `target/geas-out` when `geas` runs from the checkout's root, which the loader then resolves against the working directory of the process that loads the module, so such a module is meant to run from that root, the way the repository's gates run. An installed `geas` with no checkout in sight links with `$ORIGIN` alone and nothing else, and the loader finds `libgeasrt.so.0` where `make install` put it, under the prefix's `lib`, through its ordinary search path. The runtime library itself carries no RUNPATH.

**SQLite inside the runtime.** The store backend is SQLite 3.46.1, compiled from the vendored amalgamation into `libgeasrt.so.0` itself, threadsafe, with extension loading left out. Its `sqlite3_*` functions are exported from the runtime's dynamic symbol table along with the `geas_*` calls, and a host may call them. They are not part of the Geas ABI: their version follows the vendored SQLite and the promise in Stability does not cover them. A host that brings a SQLite of its own into the same process and loads the runtime into the global scope has two definitions of every `sqlite3_*` symbol, and the dynamic loader decides which one each reference binds to by load order; a host that wants the two kept apart opens the runtime with `RTLD_LOCAL`, the way the Python binding does, or links its own SQLite statically with its symbols hidden.

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
    uint32_t              nattrs;
    const GeasAttrDesc*    attrs;
    /* the course tail, see The course */
    GeasCourseFn          course_fn;
    uint32_t              course_nslots;
    uint64_t              course_hash;
    /* the amends tail, see Amendment */
    uint32_t              namends;
    const GeasAmendDesc*  amends;
    /* the type table, see The checked boundary */
    uint32_t              ntypes;
    const GeasTypeNode*   types;
    const GeasItemTypes*  item_types;
    const uint32_t*       vow_types;
    const GeasItemTypes*  remedy_types;
    /* the course slot types, see The course */
    const uint32_t*       course_slot_types;
    /* the foreign symbols, see Foreign symbols */
    const char* const*    pledge_symbols;
    const char* const*    remedy_symbols;
} GeasContractDesc;
```

That is the whole struct, and its order is final: every field above keeps its offset for the life of the 1.x ABI, and anything the descriptor gains later appends after `remedy_symbols`. The tails arrived in this order, each one closing on a size macro the registrar reads, see Descriptor size: the store surface through `sub_flags`, the workflow surface through `term_ms`, the declared attributes through `attrs`, the course through `course_hash`, the amends blocks through `amends`, the type table through `remedy_types`, and the course slot types and the foreign symbols through `remedy_symbols`.

Pledges and vows appear in declaration order. A vow whose declaration carried an initializer has `has_default` set and the literal encoded in `default_value`; a string default points at bytes inside the module. Signing copies every vow value onto the instance, so nothing an instance reads aliases the descriptor after sign.

`subs` names every subcontract in declaration order, an anonymous one as a NULL entry, and each pledge's `sub` indexes it, -1 for a pledge declared outside any subcontract. A `sub` outside `[0, nsubs)` reads as loose, which is what a zero-filled handwritten descriptor gets. The atom table, the `has_reqs` flag, and the three postfix policy programs are the requirements surface, described in their own section below; a handwritten descriptor that carries none of them gets the structural default policy.

The schema surface and the subcontract flags append at the tail, so a zero-filled handwritten descriptor and every module compiled before the store layer read as `nschemas` 0 with a NULL `sub_flags`: no schema, not store-backed, and every subcontract loose. `schemas` carries one `GeasSchemaDesc` per schema a store-backed contract declared, each naming a table, its column count, and its columns in declaration order. An `GeasSchemaCol`'s `ty` is one of the seven scalar tags a flat row holds, `Int`, `UInt`, `Float`, `Bool`, `Byte`, `Char`, or `String`; a composite never reaches a column, refused at the type checker before a descriptor is emitted. The first column is the schema's primary key, the convention the store operations key against. The runtime reads the schemas at sign to reconcile the tables and to route a store operation against the right one. `sub_flags` is one byte per subcontract, indexed by the same ordinal a pledge's `sub` field and a sub atom resolve against, carrying the `GEAS_SUB_*` bits:

```c
enum { GEAS_SUB_TRANSACTIONAL = 1 };
```

`GEAS_SUB_TRANSACTIONAL` marks a subcontract whose pledges run as one all-or-nothing episode: the runtime opens a transaction on the first fulfillment of one of its pledges, commits the instant the subcontract completes, and rolls back on the first `Err` or at a break before the commit. The bits ride their own array so the subs table and every descriptor emitted before the store layer stay byte identical, and a NULL `sub_flags` reads as all zero, the loose grouping every earlier contract carried. Both tails are pure additions, never a change to an existing field, and the shape hash covers what they say: a schema through its `schema` segment and the transactional modifier through the `sub` segment of its subcontract, see The shape hash. The workflow tail after them, the kinds, terms, remedies, and contract term, follows the same rule and has its own section, The workflow surface.

## Declared attributes

So a declared attribute is a host setting carried by the module, available
before sign and on an instance. The descriptor appends `nattrs` and `attrs`
after `term_ms`. A contract with no declared settings emits no attribute table
and no new initializer fields. Builtin keys keep their existing meanings and
do not enter this table. Declared settings stay outside the shape hash and
mangled names, so changing one leaves a parked instance compatible.

```c
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

typedef struct GeasAttrInfo {
    uint32_t owner;
    const char* owner_name;
    const char* key;
    const GeasValue* value;
} GeasAttrInfo;
```

A contract owner has index zero, a subcontract indexes `subs`, pledges and
signals index `pledges`, and a remedy indexes `remedies`. `Int` and size
literals carry `GEAS_TY_INT` in `as.i`, a size counting bytes, and `Duration`
uses the same tag and arm in milliseconds. `Float` uses `GEAS_TY_FLOAT` and
`as.f`, `Bool` uses `GEAS_TY_BOOL` and `as.b`, and `String` uses
`GEAS_TY_STRING` and `as.s`, pointing at module bytes. The value's variant tag
is zero. On LP64, `GeasAttrDesc` is 56 bytes and `GeasAttrInfo` is 32 bytes,
with natural C alignment.

```c
GeasStatus geas_attr_count(GeasRuntime*, const char* contract, size_t* count);
GeasStatus geas_attr_at(GeasRuntime*, const char* contract, size_t index,
                        GeasAttrInfo* out);
GeasStatus geas_contract_attr(GeasRuntime*, const char* contract, const char* key,
                              const GeasValue** out);
GeasStatus geas_item_attr(GeasRuntime*, const char* contract, const char* item,
                          const char* key, const GeasValue** out);
GeasStatus geas_instance_attr_count(const GeasContract*, size_t* count);
GeasStatus geas_instance_attr_at(const GeasContract*, size_t index, GeasAttrInfo* out);
GeasStatus geas_instance_attr(const GeasContract*, const char* item, const char* key,
                              const GeasValue** out);
```

Enumeration visits contract settings, pledges, subcontracts, signals, then
remedies, in each table's declaration order and each list's written order.
`owner_name` names that declaration, NULL for an anonymous subcontract. An
item lookup names a pledge, signal, named subcontract, or remedy. On the
instance lookup a NULL `item` selects the contract. Unknown contract or item
names and an enumeration index past the end return `GEAS_ERR_NAME`. A key the
owner does not carry returns `GEAS_OK` with `*out` NULL. NULL required arguments
return `GEAS_ERR_TYPE`. Output pointers and counts are cleared before a failed
lookup. The returned names, values, and string bytes are borrowed, immutable,
and valid until runtime shutdown, including after an instance breaks.
Anonymous subcontract attributes remain available through enumeration.

Python exposes `rt.attrs(contract)` as a list of dictionaries with `owner`,
`owner_name`, `key`, and a decoded `value`, and `rt.attr(contract, key,
item=None)` returns a decoded value or `None` for an absent key. A signed
contract also provides `attrs()` and `attr(key, item=None)`. Owner kinds are
`contract`, `subcontract`, `pledge`, `signal`, and `remedy`. Python copies
strings and values, so the returned objects survive shutdown.

## The registrar

A module exports exactly one undecorated symbol:

```c
GeasStatus geas_module_register(GeasRuntime* rt);
```

`geas_module_load` dlopens the module with `RTLD_NOW | RTLD_LOCAL`, resolves the registrar, and calls it. The registrar hands each contract descriptor to `geas_register_contract` and stops at the first failure. A dlopen that fails and a module with no `geas_module_register` are `GEAS_ERR_LOAD`; a registrar that fails answers its own status, and the module is closed.

**What the registrar checks.** Before anything registers, the runtime holds the descriptor to its structure, and the first check that fails is the status, `GEAS_ERR_TYPE` unless named otherwise:

- a NULL runtime or descriptor, a size below `GEAS_CONTRACT_DESC_V1_SIZE`, or a NULL contract name;
- a pledge, vow, remedy, or attribute table that is NULL while its count is not, and a NULL name in any of them; an atom table, a policy program, or an amends table that is NULL while counted; a schema table, a schema's table name, or a column name that is NULL;
- an attribute row whose owner index is out of range for its owner kind, a pledge row named as a signal's owner or the other way round, a nonzero variant tag, or a value that is not an `Int`, a `Float`, a `Bool` of 0 or 1, or a `String` with bytes behind its length;
- an amends table that does not hold up, see Amendment, with `GEAS_ERR_OOM` when the scratch the check needs cannot be had;
- a type table that does not hold up, see The checked boundary;
- course slot types without a course, a slot count, or a type table, or with a slot whose type has no value of at most 256 levels, see The course;
- a foreign symbol on a signal, or on a pledge or remedy that carries a compiled body, see Foreign symbols.

Then, under the runtime lock, a frozen runtime is `GEAS_ERR_STATE`, a contract name the runtime already holds is `GEAS_ERR_NAME`, a mangled name another entry already keys, in this contract or another, is `GEAS_ERR_NAME` with nothing of the contract registered, and a table that cannot grow is `GEAS_ERR_OOM`. The pledge kinds, the terms, and the remedy targets are checked at every sign, resume, and claim instead, see The workflow surface, and a pledge's `sub` index outside the subs table reads as loose.

**Tables grow.** The runtime holds no table of fixed size: the instance table, the registered descriptors, the iname table, the bindings, and the loaded modules all double from sixteen entries as they fill, and futures, the pool queue, and an instance's pending events are lists. A runtime holds as many instances as memory does, and growth that fails is `GEAS_ERR_OOM`. The fixed numbers the runtime does keep are named where they apply: the pool size, at most 256 workers; the value depth of 256 at every boundary and 4096 for a deep copy; and the eight rounds a claim retries a rewritten row.

The workflow prefix has size `GEAS_CONTRACT_DESC_V2_SIZE`, through `term_ms`.
The sized registrar accepts that prefix with no attributes. It treats an
incomplete attribute tail as absent, and validates every complete attribute
row's owner index and scalar type before registering it. Both the V1 symbol
and shorter sized descriptors continue to load without reading past their
reported size.

**Descriptor size.** The contract descriptor grows at its tail, so the registrar says how much of it the caller has:

```c
GeasStatus geas_register_contract(GeasRuntime* rt, const GeasContractDesc* desc);
GeasStatus geas_register_contract_sized(GeasRuntime* rt,
                                      const GeasContractDesc* desc,
                                      size_t desc_size);

#define GEAS_CONTRACT_DESC_V1_SIZE \
    (offsetof(GeasContractDesc, sub_flags) + sizeof(const uint8_t*))
#define GEAS_CONTRACT_DESC_V2_SIZE \
    (offsetof(GeasContractDesc, term_ms) + sizeof(uint64_t))
#define GEAS_CONTRACT_DESC_V3_SIZE \
    (offsetof(GeasContractDesc, attrs) + sizeof(const GeasAttrDesc*))
#define GEAS_CONTRACT_DESC_V4_SIZE \
    (offsetof(GeasContractDesc, course_hash) + sizeof(uint64_t))
#define GEAS_CONTRACT_DESC_V5_SIZE \
    (offsetof(GeasContractDesc, amends) + sizeof(const GeasAmendDesc*))
#define GEAS_CONTRACT_DESC_V6_SIZE \
    (offsetof(GeasContractDesc, remedy_types) + sizeof(const GeasItemTypes*))
#define GEAS_CONTRACT_DESC_V7_SIZE \
    (offsetof(GeasContractDesc, remedy_symbols) + sizeof(const char* const*))
#define geas_register_contract(rt, desc) \
    geas_register_contract_sized((rt), (desc), sizeof(GeasContractDesc))
```

Each size names the end of one tail: V1 the store surface, V2 the workflow surface, V3 the declared attributes, V4 the course, V5 the amends blocks, V6 the type table, and V7 the course slot types and the foreign symbols, the whole struct as 1.0.0 ships it. On LP64 they are 144, 192, 208, 232, 248, 288, and 312 bytes, the last one `sizeof(GeasContractDesc)`.

`geas_register_contract_sized` copies `desc_size` bytes of the top level struct, capped at the size this runtime knows, into a struct of its own and reads every field from that copy, so a field past the caller's size reads as zero and a field past the runtime's is ignored. A size below `GEAS_CONTRACT_DESC_V1_SIZE` is `GEAS_ERR_TYPE`. A size that reaches through `remedy_types` brings the type table with it and the registrar checks the table before anything registers, see The checked boundary; `GEAS_CONTRACT_DESC_V5_SIZE` and below carry none, and a size that ends before `remedy_symbols` carries no course slot types and no foreign symbols, which read as NULL. The exported symbol `geas_register_contract` is what every module built before the workflow tail links against, and it reads exactly the V1 prefix, the fields through `sub_flags`, so such a module loads into this runtime with no byte past its descriptor touched and behaves as it always did. Code compiled against the current header gets the macro, which routes the same spelling through the sized form with the size the compiler knows, so the C a module is emitted as needs no change to pass its size, and a handwritten descriptor gets its whole struct read. There is no ABI version symbol and the module exports nothing new: the symbol a module binds to is the version. A host that reaches the registrar by symbol name, through `dlsym` or ctypes, gets the V1 reader and calls `geas_register_contract_sized` itself when it has a tail to pass.

## The checked boundary

So a contract's code reads what it is handed as the type it declared, a field of a field, the third element of a list, the payload of the variant a match picked, and it reads it with no check of its own. That is only sound when every value that reaches it really is that type, and a value can come from outside the compiler's sight: a C host builds one by hand, a host bound body answers one, a park row comes back from a store anyone could have written to. Now the runtime holds every such value to its declared type all the way down, at the moment it crosses, and a value that does not conform is `GEAS_ERR_TYPE` before any code that would read it runs. The compiler describes every type a contract's boundary carries in a type table at the tail of the descriptor, and the runtime walks the value against the table.

**Where it holds.** Every place a value enters the contract's code or the runtime's keeping:

| boundary | held to | when | a value that does not conform |
|---|---|---|---|
| a fulfillment's arguments, by value and by reference | the pledge's parameter types | on the instance's deep copy, after copy in, before the body | `GEAS_ERR_TYPE`, no body run, no latch moved, the frame taken back off the heap |
| a body's answer, compiled or host bound | the pledge's declared return | after every attempt, before the latch | that attempt's `GEAS_ERR_TYPE`, retried like any failed attempt, never latched |
| a course's calls, `geas_course_call` and a together's | the pledge's parameters and return, as above | the same moments as a host fulfillment | the call's `GEAS_ERR_TYPE`, and the course halts on it |
| a signal's payload | the signal's parameter types, Unit for none, the value for one, a Tuple for several | on the instance's copy, before the latch, at the first delivery | `GEAS_ERR_TYPE`, nothing latched, no payload stored |
| a vow at sign, a default as much as an override | the vow's declared type | on the instance's copy, before the instance exists | `GEAS_ERR_TYPE`, no instance |
| a vow an amends block computes | the vow's declared type at the new version | after the block ran, before the instance exists | `GEAS_ERR_TYPE`, no instance |
| a remedy's argument | the remedy's declared parameter | before the body | the remedy fails with `GEAS_ERR_TYPE`, its body never run |
| a remedy's answer | the remedy's declared return | after every attempt | that attempt's `GEAS_ERR_TYPE`, no Err payload kept |
| a park row at resume, claim, and amend | the vows, every Ok value and Err payload, every signal payload, every failed remedy's Err, a together's outcomes against their pledges, and every slot of a course frame against its slot type | after the decode, before the instance is published | `GEAS_ERR_TYPE`, no instance, and a claim leaves the row in the store |

A signal delivered again after it latched still changes nothing and answers `GEAS_OK`, payload unread, since nothing of it is kept. A course slot is held to its entry in `course_slot_types`, see The course, and a slot whose entry is `GEAS_NO_TYPE`, or a frame whose descriptor carries no slot types, is held to the structural facts below. A descriptor with no type table at all holds a park row to the wire decoder's own checks alone, see The value wire codec.

**What conforms.** The walk takes a value and a type and holds:

- The value's `ty` is the type's tag. A `Duration` and a `Time` are `GEAS_TY_INT`, there is no tag of their own.
- `tag` is 0 on anything but Option, Result, and a declared sum.
- A `Bool` is 0 or 1. A `Char` is a Unicode scalar value, at most U+10FFFF and outside the surrogate block U+D800 through U+DFFF. `Int`, `UInt`, `Float`, `Byte`, and `Unit` hold any bits their arm can.
- A `String` is valid UTF-8: no overlong encoding, no encoded surrogate, nothing past U+10FFFF, no sequence the length cuts short, and an embedded zero is ordinary ASCII. A `String` or `Bytes` with a nonzero `len` has a non NULL `ptr`.
- A `List` has `len` at most `cap`, data behind a nonzero `len`, an `elem_ty` that is the element type's tag, and every element carrying `elem_ty` and conforming to the element type.
- A `Map` has an even slot count, `len` at most `cap`, data behind it, an `elem_ty` that is the key type's tag and one of the six keyable scalars, every key carrying it and conforming to the key type, every value conforming to the value type, and every key distinct by `geas_value_eq`.
- A `Tuple` and a record have exactly as many slots as the type has, each conforming to its own slot's type, data behind a nonzero length.
- A declared sum has a `tag` below its variant count and exactly the variant's field count, each field conforming. A bare variant carries `len` 0.
- An `Option` has `tag` 0 with a NULL box, or `tag` 1 with a box holding a value of the inner type.
- A `Result` has `tag` 0 or 1 and a box holding a value of the Ok or the Err type, or a NULL box when that arm is Unit.
- A recursive type is held at every level the same way.
- `GEAS_TY_PLEDGE_REF`, `GEAS_TY_INSTANCE`, and any tag past the enum never conform.

A slot of the table that reads `GEAS_NO_TYPE` holds the value to the structural facts every value keeps whatever its type, the list above with every comparison against a declared type dropped: the scalar ranges, valid UTF-8, data behind a length, `len` within `cap` on a list and a map, a list's elements carrying its `elem_ty`, a map keyed by a keyable scalar with distinct keys, a tag only on a sum shaped value, None with no box. Everything that conforms in either mode is a value the wire codec encodes, so whatever a boundary accepts can always be parked and read back.

**The depth limit.** A value nests at most `GEAS_VALUE_MAX_DEPTH` levels, 256, counted in values from the outermost down to the innermost, both included, so an Int is one level and a `List<Int>` holding one is two. A value of 256 levels conforms, parks, and resumes; one of 257 is `GEAS_ERR_TYPE` at every boundary, the walk stopping at the limit rather than following the value down, so a value nested ten thousand deep costs no more than one nested 257. The wire codec holds the same line through `GEAS_WIRE_MAX_DEPTH`, which is the same constant, so a value that crosses always encodes. Each level costs the walk and the codec a few hundred bytes of stack at most, well under a megabyte at the limit, on the caller's thread or a pool worker's. `geas_value_deep_copy` takes values deeper than this, to 4096 levels, since a copy on the instance's own heap crosses no boundary; the line every boundary holds is 256.

**The node budget.** A value holds at most `GEAS_VALUE_MAX_NODES` nodes, 2^24, every value inside it counted once per path that reaches it, so a value that shares its substructure many times over counts as the tree it unfolds to, which is what it would cost to copy or encode. It is a line on resources and not on types, so a value past it is `GEAS_ERR_OOM` and not `GEAS_ERR_TYPE` wherever the runtime meets it: at every boundary in the table above, in `geas_value_deep_copy`, and in the wire encoder, which a park goes through, so a park of an instance holding such a value answers `GEAS_ERR_OOM` and writes nothing. The walk counts as it goes and stops at the budget, so a value of a billion shared nodes costs no more to refuse than one just past it. `geas_list_push` and `geas_map_set` hold the same line from the other side, `GEAS_ERR_OOM` for an element or a pair that would take the container past it, so a value a host or a body grows one element at a time never reaches a size a boundary would refuse.

**The type table.** The table rides at the tail of the descriptor, after the amends blocks:

```c
#define GEAS_NO_TYPE 0xFFFFFFFFu
#define GEAS_VALUE_MAX_DEPTH 256u
#define GEAS_VALUE_MAX_NODES (1ull << 24)
#define GEAS_SLOT_MIN_NODES_MAX 4096u

typedef struct GeasTypeVariant {
    const char*      name;
    uint32_t         nfields;
    const uint32_t*  fields;      /* indices into the contract's types table */
} GeasTypeVariant;

typedef struct GeasTypeNode {
    uint32_t                tag;
    uint32_t                nkids;
    const uint32_t*         kids;
    const char*             name;       /* record or sum name, NULL otherwise */
    uint32_t                nvariants;  /* sums only */
    const GeasTypeVariant*  variants;
} GeasTypeNode;

typedef struct GeasItemTypes {
    uint32_t         nparams;
    const uint32_t*  params;      /* indices into the types table */
    uint32_t         ret;         /* GEAS_NO_TYPE for a signal */
} GeasItemTypes;
```

`types` holds `ntypes` nodes, each distinct type the contract's boundary carries once, and every index anywhere in the table points into it. A node's `tag` is the `GeasTypeTag` a conforming value carries. Its `kids` depend on the tag: a List's one is its element, a Map's two are its key and its value, an Option's one is its inner type, a Result's two are its Ok and its Err, a Tuple's are its slots in order, a record's its fields in declaration order, and every other tag has none. A sum carries no kids and one `GeasTypeVariant` per variant in declaration order, the order its tags count in, each variant's `fields` the indices of its payload fields. A recursive type refers back to its own index, or to an ancestor's, and that is all recursion takes. `name` is the record's or the sum's declared name, informational.

`item_types` holds one entry per row of the `pledges` table in that table's order, signals included, each with the pledge's parameter count and types and its return; a signal's `ret` is `GEAS_NO_TYPE`, it returns nothing. `vow_types` holds one index per vow. `remedy_types` holds one entry per remedy, its `params` the zero or one parameter the remedy takes and its `ret` the remedy's return. `GEAS_NO_TYPE` in any of those slots says the table has no spelling for the type, which the compiler emits only for a name it has no layout for, and such a value is held to the structural facts.

`geas build` emits one table per contract, named after the contract with its name's length in front, `k5_Order_types` and `k5_Order_item_types` for `Order`, so two contracts whose names run into each other's suffixes never share a symbol, and every other per contract symbol a module defines follows the same stem.

**At registration.** The registrar checks the whole table before the contract is registered, so the walk can trust every index it follows: every node's tag one a boundary carries, never `GEAS_TY_PLEDGE_REF` or `GEAS_TY_INSTANCE`; every kid in range and of the count its tag takes; a map's key a keyable scalar; a sum with at least one variant and every field in range; every item, vow, and remedy slot an index or `GEAS_NO_TYPE`, with counts that match the descriptor's own; a vow's node tag equal to the vow's `ty`; and no return on a signal. A table that does not hold up is `GEAS_ERR_TYPE` and the module fails to load.

**A descriptor with no table.** `ntypes` 0 is a descriptor with no table, which is what a handwritten descriptor and a module built before the table carry, and so is a descriptor whose reported size ends before `remedy_types`, `GEAS_CONTRACT_DESC_V5_SIZE` and below. Such a contract behaves as it always did: the runtime checks the argument count and a vow's top tag, a compiled thunk checks each argument's tag, and nothing below a top tag is held to anything.

**Pledge values.** Pledge values are not part of this version of the language: the checker refuses a pledge type, `pledge(Int) -> Unit`, and a bare `instance.pledge_name` with no call, wherever either appears, so no compiled code holds one, no table spells one, and no host is ever handed one or asked for one. The tag `GEAS_TY_PLEDGE_REF` keeps its number all the same, and the registrar refuses a node that spells it, the walk refuses a value that carries it, and the wire codec refuses to encode it.

**None.** A None carries a NULL box and nothing else, so everything that conforms is something the wire codec parks. A value the runtime copies before it checks, an argument, a signal payload, a vow, has had any box behind a None dropped by the copy unread, so a host that left one there sees its call go through. A body's answer is not copied, it is checked where the body wrote it, so a None with a box behind it in an answer is `GEAS_ERR_TYPE`.

**A malformed answer.** A body that answers a value its declared return does not admit has failed that attempt with `GEAS_ERR_TYPE`. The value is dropped and never latched, so no Ok value, no Err payload, and no remedy argument is ever built from it. A pledge with a `retry` tries again as it would after any failed attempt, and a later attempt that answers a conforming value latches as usual. One with no retry left answers `GEAS_ERR_TYPE` to its caller, the pledge still pending, so the host can fulfill it again. A course whose `geas_course_call` meets that status halts with it, `GEAS_COURSE_HALTED` with status `GEAS_ERR_TYPE` on `geas_course_info`, the step after it never reached, which is what a course does with every status a call answers other than `GEAS_OK`. A remedy's malformed answer fails that attempt the same way, and with no retry left the remedy is recorded failed with `GEAS_ERR_TYPE` and the pass moves on.

## Signing and vows

`geas_contract_sign` finds the contract by name, checks the shape hash when the caller supplied one, and builds the instance's vow storage: declared defaults first, then the caller's `GeasVowBinding` overrides. An override naming no vow is `GEAS_ERR_NAME`, an override whose value carries the wrong type tag is `GEAS_ERR_TYPE`, a vow whose copy, a default's or an override's, does not conform to its declared type is `GEAS_ERR_TYPE`, and a vow with neither a default nor an override is `GEAS_ERR_UNBOUND`. A contract with any abstract pledge or abstract remedy, an `fn` of NULL nothing has bound, refuses to sign with `GEAS_ERR_UNBOUND`; a signal's NULL `fn` is by design and refuses nothing. A descriptor whose workflow tables the runtime will not run is `GEAS_ERR_TYPE`, the cases are listed under The workflow surface. No failure leaves an instance behind.

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

## Foreign symbols

So an abstract pledge or remedy can also name the C function that implements it, `[abi: "c", symbol: "charge_card"]` in the source, and then a host that exports a function under that name needs no bind call at all. The names ride the last tail of the descriptor:

```c
    const char* const*    pledge_symbols;   /* one per pledges table entry */
    const char* const*    remedy_symbols;   /* one per remedy */
```

Each table holds one entry per row of its table, the symbol for an abstract entry the source bound to one and NULL for every other row, and a table is NULL when none of its rows names a symbol. The compiler refuses a symbol that is not a C identifier and one that starts with `geas_`, `__geas_`, or `GEAS_`, the prefixes the runtime's own calls and the code geas emits are named under, so a symbol can never answer with a runtime function. It checks nothing else: a symbol that names a libc function or anything else the process links resolves to that function, and choosing a name no library already exports is the host's part. The registrar refuses a symbol on a signal and on a row that carries a compiled body.

At every sign, resume, and claim the runtime resolves each pledge and remedy in one order: a binding made through `geas_pledge_bind` or `geas_pledge_bind_user` first, then the compiled body, then `dlsym(RTLD_DEFAULT, symbol)`. Since a symbol only rides an entry with no body, that reads as a host binding beating the symbol, and the symbol answering when nothing is bound. The function has the `GeasPledgeFn` signature and runs in the same frame a bound function does. Nothing is cached, the lookup runs again at every sign, resume, and claim, and a symbol the process does not export leaves the entry unbound, so the sign, the resume, or the claim answers `GEAS_ERR_UNBOUND`, a claim leaving the row in the store.

The lookup sees what the process exports into its global scope. A host executable exports its own functions by linking with `-rdynamic`, and a shared object exports its functions by being loaded with `RTLD_GLOBAL`. A module the runtime loads is opened `RTLD_LOCAL`, so a function defined in one `.geas.so` never answers a symbol named in another, and a host whose functions live in a library it opened locally binds them with `geas_pledge_bind` instead. The Python binding is that case: it opens `libgeasrt` with `RTLD_LOCAL` and its callbacks are ctypes trampolines no symbol names, so a Python host always binds.

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

`geas_future_release` releases the receipt after wait, including a wait that returned an error, or after an explicit break forfeited it. A pending receipt or a completed but unwaited result is refused with `GEAS_ERR_STATE`; that refusal keeps the handle valid, so the host waits and releases it, or breaks the instance and releases it. Polling alone leaves the receipt unwaited. Park accepts a receipt waiting out a pledge retry or driving a remedy pass, and otherwise refuses it until delivery. A NULL future is `GEAS_ERR_TYPE`.

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

`host_ptr` addresses the host's storage for a value of type `ty`: the raw scalar for the numeric and character types, an `GeasString` struct for String or Bytes. Those are the types that cross by reference in v1; a ref carrying anything else is refused with `GEAS_ERR_TYPE`, as is a null `host_ptr`.

The protocol has exactly two moments, both on a thread the host is blocked in a geas call with, so the runtime never touches host memory behind the host's back:

1. Copy in, at fulfillment entry, inside `geas_pledge_fulfill` or `geas_pledge_fulfill_sync` on the caller's thread. The referenced value is deep copied onto the instance as a mutable slot appended after the value arguments. From here the pledge body mutates instance memory only.
2. Write back, at delivery, inside the `geas_future_wait` that collects the outcome or before `geas_pledge_fulfill_sync` returns, on the waiting thread. Each slot's final value goes back to host memory: through `write_back` when the host supplied one, `user` passed through untouched, or by the default otherwise.

The default write back writes scalars in place and writes a whole `GeasString` struct for String or Bytes. The bytes it points at are instance owned and die at break; a host that wants the shouted bytes to outlive the instance supplies a `write_back` that copies them out, which is also where a capacity declared in `cap` gets honored. Write back happens only when the fulfillment itself reported `GEAS_OK`; a thunk error leaves host memory untouched. Every slot's type is checked against its ref before anything is written, so a pledge that swapped a slot's type writes nothing and the wait reports `GEAS_ERR_TYPE`. On the way in a by reference slot is an argument like any other, its copy held to the parameter's declared type with the value slots, so a `Bool` byte of 2 or a `String` that is not UTF-8 behind a ref is `GEAS_ERR_TYPE` with no body run and nothing written back.

A `write_back` callback runs after the default write backs of the same call, in ref order, with every runtime lock let go. The wait marks the future delivered and drops its own mutex, checks every slot's type, writes each ref that has no callback in place under the instance lock, sets aside a copy of each remaining slot's value, and only then lets the instance lock go and calls the callbacks, so on a host thread a callback holds no runtime lock at all. So a callback may call into the runtime freely, the same instance included: it can read the instance, fulfill, signal, park, or break it, and wait on other receipts. The value a callback is handed is a copy the runtime made for this call, the bytes of a `String` or `Bytes` included, so it stays valid until the callback returns whatever the callback does to the instance meanwhile, a break included, and is gone after it; a callback that keeps the bytes copies them into memory of its own. A copy the runtime cannot allocate is `GEAS_ERR_OOM` with nothing written. Until the last callback of the call returns the instance counts a write back as running, so `geas_contract_free` answers `GEAS_ERR_STATE` for it, and `geas_runtime_shutdown` called from a callback is refused, see Runtime lifecycle. A wait that finds the instance already reclaimed when it comes to write back writes nothing, calls no callback, and answers `GEAS_ERR_STATE`.

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

The host clock may answer any `int64_t`, negative values included, since a test that starts its fake clock at zero and subtracts is as legitimate as wall time: every deadline and stamp is computed with saturating arithmetic, a park row carries a negative stamp as it is, and the one value the runtime keeps for itself is `INT64_MIN`, which a stamp spells as not yet eligible. The signing second is the clock's reading divided by 1000, truncated toward zero.

The runtime's own timed waits never read the host clock, they run on `CLOCK_MONOTONIC`, so a wall clock stepped by NTP or an operator moves no wait: the default wait between a pledge's retries, the timeout of `geas_course_wait`, and the ticker's interval all count real elapsed time. `geas_future_wait` has no timeout and waits until the outcome exists. The deadlines the clock does drive, a `within`, a contract term, a course `sleep` or `select` timeout, are compared against the host clock at each tick.

The runtime runs no timer unless a host asks for one. Deadlines are checked lazily: every entry point that reads or moves an instance runs the tick first, which is `geas_pledge_fulfill` and the worker that runs it, `geas_pledge_fulfill_sync`, `geas_signal`, `geas_contract_state`, every call of the partial surface, `geas_instance_park`, and `geas_future_wait`, the last only when the instance lock is free so a waiter never queues behind an instance. A host that wants a lapse to happen when the deadline passes and not when the next call arrives calls `geas_instance_tick` on one instance or `geas_runtime_tick` over every instance the runtime holds, from whatever timer it has.

```c
GeasStatus geas_runtime_start_ticker(GeasRuntime* rt, uint32_t interval_ms);
GeasStatus geas_runtime_stop_ticker(GeasRuntime* rt);
```

A host with no timer of its own starts the runtime's ticker, one runtime thread that calls `geas_runtime_tick` every `interval_ms` of real time, which is what lets a course's `sleep`, a `select` timeout, and a `within` fall due on time with nobody calling in. The interval is real time even when the clock is a host's own, so a ticker over a fake clock ticks as often as the interval says and applies whatever the fake clock reads. A second start while one runs is `GEAS_ERR_STATE`, an interval of 0 or a NULL runtime `GEAS_ERR_TYPE`. Stop joins the thread before it returns and is `GEAS_ERR_STATE` when no ticker runs, and shutdown stops a running ticker first. The ticker cannot stop itself: a stop called on the ticker's own thread, from a hook its tick delivered for instance, would join the thread it runs on, so it answers `GEAS_ERR_STATE` and the ticker runs on, and so does a stop from any thread holding an instance lock. A host stops the ticker from a thread of its own.

A tick applies every deadline that has passed in deadline order, earliest first, a pledge before a subcontract before the contract term on a tie, and evaluates the policy after each one, so the contract moves exactly as it would have had each lapse been seen the instant it fell due. It stops the moment the contract leaves Signed, Partial, and Fulfilled, since a deadline that fell after a break is moot. If the tick decides a break, the remedies run inside it, on the calling thread, before it returns. A tick called from inside a pledge or remedy body on that body's own instance does nothing: a contract does not break underneath a body it is running.

## Durations and times

So a `Duration` and a `Time` are just numbers on the wire, and both cross as `GEAS_TY_INT` in `as.i`, with no type tag of their own. A `Duration` is a count of milliseconds, the same unit every `within`, `every`, `cap`, and `term_ms` in the descriptor tables already speaks, and a `Time` is milliseconds since the Unix epoch, which is what the runtime clock answers, so a `Time` a host hands in and a deadline the runtime computes are directly comparable. A literal like `30m` lowers to the Int `1800000`, `Time - Time` is the `Duration` between them, and a `Duration` vow, parameter, record field, or return is carried exactly where an `Int` one would be, so a C host reads `v.as.i` and a Python host gets a plain int.

The type lives in the checker and in the shape hash, where `Duration` spells `D` and `Time` spells `W`, so a pledge that takes a `Duration` and one that takes an `Int` hash apart and mangle apart even though their frames look the same at runtime. The gRPC surface follows the wire: both are `int64` fields, and a wrapper message built around one takes its Geas name, a `DurationOption` or a `TimeList`.

## Terms

**Eligibility.** Every item carries the moment it became eligible. An item with no `after` is eligible at sign. An item with an `after` becomes eligible when every item it names has latched fulfilled, a named subcontract counting as fulfilled when every pledge inside it has, and the stamp is the clock's reading at that latch. An Err does not satisfy an `after`, only a fulfilled latch does, so an item waiting on something that broke or lapsed never becomes eligible. A subcontract's `after` applies to every pledge and signal inside it: a member is eligible once its subcontract is and its own `after` has landed as well. A stamp is written once and never moves. The checker rejects cycles; a handwritten cycle is simply never eligible.

**after.** A fulfillment of a pledge that is not yet eligible is `GEAS_ERR_STATE` and the body does not run. Nothing is latched, the call can be made again once the items it waits on have landed.

**within.** The deadline is the eligibility stamp plus `within_ms`. A pledge or signal still pending when it passes lapses: its latch moves to broken, no Err payload is stored because the pledge's own error type has no say in a deadline, a flag records that the latch is a lapse, and the policy is evaluated, a lapse arming the break line like any broken latch. A subcontract's `within` counts from the subcontract's own eligibility, and when it passes with the subcontract not wholly fulfilled, every pledge inside it that has not latched lapses; the ones that latched keep their latch. A latch that landed in time never lapses, however far the clock runs.

A fulfillment that arrives after the lapse is `GEAS_ERR_STATE` and its body does not run, the gate's tick lapsing the pledge first if nothing had yet. The deadline bounds when an attempt may start: a body that was started in time lands its outcome even if it returns after the deadline, since the work it did is real and a fulfilled latch is what lets a remedy undo it.

**retry.** A fulfillment whose body fails is run again inside the same fulfill call, up to `retry_count` more times. A failure is a thunk status other than `GEAS_OK`, the lifecycle failure, which always earns a retry while any remain, or an Err whose payload is a `GEAS_TY_SUM` value with a variant tag listed in `retry_on`. With no `retry_on` a business Err is an outcome and is never retried, and an Err whose payload is not a declared sum never matches a list. Before retry number n, counting from 0, the worker waits `retry_every_ms * retry_backoff^n` milliseconds, held at `retry_cap_ms` when that is nonzero, through the runtime's sleep function. The delay is worked out by squaring over the bits of n, at most 32 steps whatever the retry count, so a pledge allowed a million retries computes its last delay as fast as its first; a backoff of 0, below 0, or NaN reads as 1.0, an `every` of 0 is no delay, a delay past the cap or past 9.0e18 ms is held there, and a delay is never longer than the time left before the pledge's own deadline. Only the last attempt's outcome is latched and delivered: a retried Err leaves no latch and no payload behind, and a fulfillment that used up its retries on a status delivers that status and latches nothing, as a status always has.

The delay is waited on the thread running the fulfillment with the instance lock released, so state reads, signals, other pledges, a park attempt, and a break all proceed while a worker sleeps. The frame is the same frame for every attempt. When the worker takes the lock back it trusts nothing: if the contract broke or was reclaimed meanwhile, or the pledge's own `within` ran out, the fulfillment stops and reports `GEAS_ERR_STATE` with no further attempt. A delay is never waited past the pledge's deadline, the worker wakes at the deadline and lapses the pledge there. A fulfillment waiting out a delay can park. Its row records the attempts already completed and the absolute time the next attempt is due on the runtime clock. The original fulfillment keeps waiting and continues normally if the host carries on, with its frame and future unchanged. A second fulfillment of that pledge is refused while its original delay is active. On resume the pledge is pending, with the same retry budget already spent; the host passes its arguments again. A fulfillment before the saved due time reports `GEAS_ERR_STATE` without running a body or consuming an attempt. At or after that time it runs the next attempt, and the remaining retries keep their original backoff positions. A tick applies deadlines but does not supply arguments or start a pending pledge. One bound to know: a synchronous fulfillment nested inside another body on the same instance holds that instance's lock recursively, and its delay is waited with the outer hold still in place. A remedy's retry delay is waited through the runtime's sleep function, the host's when it set one, and no cancel request wakes it.

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

The first delivery wins. A delivery of a signal that already latched returns `GEAS_OK` and changes nothing, the latch and the payload stay the first one's, so a host passing a retried webhook along need not deduplicate first. A signal whose `after` has not landed is `GEAS_ERR_STATE` and latches nothing. A signal whose `within` ran out is latched broken by the lapse and a later delivery is `GEAS_ERR_STATE`. A delivery to a contract that is not Signed, Partial, or Fulfilled is `GEAS_ERR_STATE`, Remedying included. An unknown name is `GEAS_ERR_NAME`, a name that is a pledge is `GEAS_ERR_TYPE`, and a wrong argument count is `GEAS_ERR_TYPE`. The checks run in one fixed order, the contract's state, the name, the kind, the argument count, the eligibility, the lapse, and the first that fails is the status. A first delivery then holds the instance's copy of the payload to the signal's declared parameters, and one that does not conform is `GEAS_ERR_TYPE` with nothing latched and nothing stored.

The two surfaces refuse each other's names. `geas_pledge_fulfill` and `geas_pledge_fulfill_sync` on a signal report `GEAS_ERR_TYPE`, through the wait for the future form, and `geas_pledge_bind` on a signal is `GEAS_ERR_TYPE` since there is no body to bind. A contract signs with its signals unbound, they are the one NULL `fn` the sign does not refuse.

**How a compiled module lays it out.** The compiler puts every signal after every pledge in the pledges table, signals in declaration order, so a pledge keeps the index it always had and a signal's index is the pledge count plus its own ordinal; the kind byte is what tells the two apart, never the position. A loose signal is a requirements atom on that row, and a contract with no requirements block counts its loose signals in the synthesized default the way it counts its loose pledges. Inside Geas a host delivers one with `instance.delivered(t)`, which is a `geas_signal` call answering Unit, and a refusal faults the calling body with that status like every other lifecycle call. On the gRPC surface each signal is its own unary rpc, `Signal` then the signal's CamelCase name, taking the instance id and the parameters and answering an empty `SignalReply`, a refusal riding the call's status, and the `ContractState` enum of a file whose contracts carry any of the workflow surface gains `REMEDYING = 5`, while a file whose contracts carry none keeps the five states it always had.

`geas_signal_payload` hands out a pointer to the stored payload: `GEAS_ERR_STATE` while the signal is undelivered or after an explicit break reclaimed the heap, `GEAS_ERR_TYPE` for a pledge's name, `GEAS_ERR_NAME` for an unknown one.

## Remedies

A remedy undoes one pledge that landed. It is a `GeasRemedyDesc` in the contract's `remedies` table, it runs in the uniform thunk frame with `ctx` the instance, and its frame is one slot holding the target pledge's Ok value when `has_param` is 1 and empty when it is 0. The Ok value is the payload of the Ok when the pledge's body answered a Result and the value itself otherwise, kept beside the latch from the moment the pledge first latched fulfilled. A remedy succeeds when its thunk returns `GEAS_OK` with anything but an Err, so a remedy returning Unit and one returning `Ok(...)` both count.

**Binding.** A remedy binds through `geas_pledge_bind` or `geas_pledge_bind_user` under `"Contract.remedy"` or its mangled symbol, a binding beating a compiled body the way it does for a pledge, resolved at sign. An abstract remedy, an `fn` of NULL nothing bound, refuses the sign with `GEAS_ERR_UNBOUND`. A remedy is never a requirements atom, never an item of the partial surface, and has no entry in the iname table.

**The pass.** When a contract breaks, by a matched break line, by a lapse that matched it, by its term running out, or by `geas_contract_break` on a contract still Signed or Partial, the runtime looks for remedies that are due. A remedy is due when its pledge latched fulfilled and it has not run. If none is due the contract goes straight to Broken. Otherwise it reads `GEAS_REMEDYING` and the pass runs: the due remedies one at a time, newest latch first by the order the pledges latched fulfilled, two remedies on one pledge in declaration order, and when the last returns the contract reads `GEAS_BROKEN`. A pledge inside a transactional subcontract whose episode did not commit, still open or rolled back, has no remedy due, nothing it wrote is durable and the rollback is its undo; the runtime records rollback separately from commit, even if every pledge latched fulfilled before the break. Open episodes roll back before the first remedy runs, so a remedy can write through another connection to the same store.

The pass can park between remedies, inside a remedy body, and during its retry delay, including inside the host sleep callback. The future whose outcome triggered the pass is accepted by park and keeps waiting in the original runtime. Once that original pass ends, wait returns the pledge's original status and value, so a business `Err` still arrives with `GEAS_OK`. Park does not cancel that future or end the live pass. The resumed copy runs only remedies that had not started, at its first tick, and ends Broken. A remedy recorded as running resumes failed with `GEAS_ERR_STATE`, including one waiting to retry.

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

`geas_remedy_state` reads one remedy by name, `IDLE` for one that has not run, which includes every remedy whose pledge never landed, and for an unknown name. The four `geas_partial_remedy_*` calls walk the failed remedies in declaration order. The status is what the last attempt's thunk returned: a lifecycle failure reads as that status, a remedy that failed by value reads `GEAS_OK` with the Err payload behind `geas_partial_remedy_error`, `GEAS_ERR_STATE` marks a remedy that was in flight when the instance was parked, and `GEAS_ERR_TYPE` marks a remedy that takes its pledge's Ok value when none was there to hand it or when the value does not conform to the remedy's declared parameter, in which case the body never ran, and a remedy whose last attempt answered a value its declared return does not admit. `geas_partial_remedy_lapsed` reads 1 when the remedy's `within` is what stopped it. An index out of range reads NULL, `GEAS_ERR_NAME`, 0, and `GEAS_ERR_NAME` in turn. The Err payload follows the instance heap like every payload, readable after an automatic break and a zeroed Unit after an explicit one, while the states and statuses stay readable either way.

**The host's break.** `geas_contract_break` on a contract that is Signed or Partial is a break like any other: the remedies run first, the state reading Remedying while they do, and the heap is reclaimed after the last one returned, since the Ok values the remedies are handed live on it. On a contract that is Fulfilled the break runs no remedy: the contract met its terms, there is nothing to undo, and the break reclaims as it always has. On a contract the policy already broke the remedies ran then, so the break only reclaims. A break that finds another thread running the pass waits for the pass to end and returns once the heap is reclaimed. A break called from inside a body running on that instance cannot run or wait out a pass underneath the body, and cannot reclaim the heap the body's frame lives on: it marks the contract, returns `GEAS_OK`, and the pass and the reclaim happen when that body's fulfillment ends, the body's own pledge latched first so its remedy runs with the rest; from inside a remedy body the reclaim simply happens when the pass ends. The same holds for a break the policy decides underneath a body, a nested fulfillment on the same instance that matched the break line: the contract reads Remedying from that moment, whether or not a remedy is due yet, and what is due is settled when the outermost fulfillment ends.

## The course

A contract that carries a `course` block is engine driven. The compiler lowers the course to one C function, a state machine over a frame, and the runtime drives that function on its pool: the course starts when the contract signs, runs the contract's own pledges inline, suspends at every wait with its thread going back to the pool, and is entered again when a signal, a deadline, or a joined call says it can move. Nothing is replayed from the top, an entry jumps to the saved step with the saved slots. A host of such a contract binds the abstract bodies and delivers signals, and that is all; `geas_pledge_fulfill` and `geas_pledge_fulfill_sync` on one of its pledges answer `GEAS_ERR_STATE` and the body never runs.

**The descriptor.** The course rides its own tail, after `attrs`, and its slot types ride a later one, after the type table:

```c
GeasCourseFn    course_fn;          /* NULL for a host driven contract */
uint32_t        course_nslots;      /* the slot count of the frame */
uint64_t        course_hash;        /* the fingerprint a park row carries */
/* ... the amends blocks and the type table ... */
const uint32_t* course_slot_types;  /* one types table index per slot */
```

A descriptor whose reported size ends before `course_hash` carries no course, so every module built before the course loads unchanged and reads as host driven.

**The course hash.** `course_hash` is FNV-1a 64, the shape hash's offset basis and prime, over three pieces run together. First the lowered course in its canonical text, the same slots, steps, and statements the debug command `geas dump-course` prints under a course's `hash:` and `entry:` lines, indented eight spaces at the outer level and four more a level, each line ending in a newline, and spelled canonically where the debug text is spelled for reading: the slot list, each slot spelled by its canonical type code and its kind with no name, `Slot(0): R(NBox,I) (temporary),`; the steps, each pledge or signal spelled by its name rather than its table index; and the statements, every expression carrying its type code, `Int(1):I`, and every string literal spelled with its byte length, `Str(5:hello)`. Then the layout segment of every declared record and sum a slot's type reaches, in name order, spelled as the shape string spells them. Then the `schema` segment of every schema the contract declares, in declaration order. A few more things are spelled by what they mean rather than by how the source wrote them: a binding a `match` or `select` pattern makes is spelled by its position among the arm's bindings, `Bind(0)`, a record literal lists its fields in the order the record declares them, a `Float` literal is spelled by its value as the shortest scientific decimal that reads back as the same double, `Float(1.0005e+03)`, an `Int` by its value whatever its radix or separators, a `Duration` in milliseconds, a `Char` by its scalar value, a generic constructor without the type the source spelled, `MapNew()` and `New(...)`, since the slot's type code already carries it, and a variant's payload without its labels, which the checker already holds to the field at each position. So the hash leaves out what the frame cannot feel, comments, whitespace, the name a `let` or a pattern binds, the order a record literal writes its fields in, the spelling of a literal, `1_000` against `1000` or `0x3e8`, `1m` against `60s`, `1.0e3` against `1000.0`, `'\u{41}'` against `'A'`, a clause's body, the terms, the requirements, and the version, and it moves with everything a resumed frame would read differently: a step added, removed, or reordered, a literal's value, a slot's type or the layout behind it, an operation or the operand it takes, a vow or a clause the course names, and the shape of a pattern. For `record Box { n: Int }` and a contract whose course is `let v = a(1)?` over `pledge a(x: Int) -> Result<Box, Int>`, the text is

```text
        slots: [
            Slot(0): R(NBox,I) (temporary),
            Slot(1): NBox (local),
        ],
        steps: [
            Step 1 call {
                pledge: a,
                arguments: [Int(1):I],
                result: Slot(0),
                question: true,
            },
        ],
        statements: [
            Resume 1,
            Store Slot(1) = Question(Slot(0):R(NBox,I)):NBox,
            Return,
        ],
```

followed by `type Box{n:I};`, and the hash is `0x42ad333066cef768`.

**Slot types.** `course_slot_types` holds one index into the contract's types table per slot, `GEAS_NO_TYPE` for a slot the table has no spelling for, and is NULL when the contract has no course or no type table. The registrar refuses a table with no course, no slot count, or no type table behind it, and a slot whose type the frame could not start at, see Slot limits below. With the table every slot starts at the smallest value of its type before the first entry: 0 for `Int`, `UInt`, `Byte`, `Duration`, and `Time`, +0.0, false, U+0000, Unit, an empty `String` or `Bytes`, an empty `List` or `Map` carrying its element or key tag, `None`, a tuple or record whose every field is at its own smallest value, the variant of a sum whose payload nests least, the first on a tie, and the arm of a `Result` that nests least, `Ok` on a tie, with no box when that arm is `Unit`. A slot with no type, and every slot of a descriptor with no slot types, starts as a zeroed Unit. Every slot a resume, a claim, or an amend brings back from a park row is held to its slot type all the way down before the instance is published, see The checked boundary.

**Slot limits.** A slot starts at the smallest value of its type every time an instance signs, so that value has to exist and has to be cheap to build. The rule follows two numbers for every type. The height is the fewest levels any finite value of the type nests: 1 for a scalar, a `List`, a `Map`, and an `Option`, whose smallest values are empty or `None`; one more than the deepest kid for a `Tuple` and a record; one more than the lower of its two arms for a `Result`, `Ok` on a tie; and one more than the variant whose deepest field nests least for a sum, a variant with no field counting 0, the first such variant on a tie. The node count of the smallest value is 1 for a scalar, a `List`, a `Map`, and an `Option`, and 1 plus the counts of the kids it carries otherwise, every kid of a `Tuple` or a record, every field of the variant the height chose for a sum, and the arm it chose for a `Result`, a `Unit` arm counting 1. A slot's type has to have a finite value at all, a record that holds itself with no `Option` or `List` between has none, its smallest value has to nest no deeper than `GEAS_VALUE_MAX_DEPTH`, 256 levels, and it has to hold no more than `GEAS_SLOT_MIN_NODES_MAX`, 4096 nodes, a line a record holding the next record twice crosses in a dozen levels. `geas build` refuses a course that holds a value of such a type with a message that says so and says to hold it behind an `Option` or a `List`, whose smallest value is one node however large the type inside, and the registrar counts the same way and refuses such a slot with `GEAS_ERR_TYPE`, so a handwritten descriptor cannot get one past it either.

**The frame.**

```c
typedef struct GeasCourseFrame {
    uint32_t   step;         /* resume point, 0 on the first entry */
    uint32_t   nslots;
    GeasValue* slots;        /* instance owned, each at its type's smallest value */
    int64_t    wait_due_ms;  /* the pending wait's deadline, 0 when none */
    uint32_t   wait_step;    /* the step that owns wait_due_ms */
    uint32_t   status;       /* the GeasStatus that stopped the course */
} GeasCourseFrame;

enum { GEAS_COURSE_DONE = 0, GEAS_COURSE_SUSPENDED = 1, GEAS_COURSE_STOPPED = 2 };
enum { GEAS_COURSE_READY = 0, GEAS_COURSE_WAIT = 1 };

typedef uint32_t (*GeasCourseFn)(void* ctx, GeasCourseFrame* frame);
```

The frame is the course's whole durable state. Every binding of the course, every loop's hidden position and collection copy, and every value that has to outlive a suspending operation lives in a slot; C locals in the emitted function hold only values that die before the next one. The slots live on the instance heap like every other value the instance makes, and a slot's value, along with every copy and temporary the course made, stays there until the instance is broken, so a course loop that runs many passes grows the instance with each one.

**The shape of an emitted course.** The function opens with a switch on `step` that jumps to the saved resume point, and every suspending operation of the source is one numbered step:

```c
static uint32_t k5_Order_course(void* ctx, GeasCourseFrame* f) {
    switch (f->step) {
        case 0: break;
        case 1: goto step_1;
        /* one case per step */
        default: f->status = GEAS_ERR_STATE; return GEAS_COURSE_STOPPED;
    }
    /* straight line code over f->slots */
    f->step = 1;
    {
        GeasStatus t0 = geas_course_checkpoint(ctx, f);
        if (t0 != GEAS_OK) { f->status = t0; return GEAS_COURSE_STOPPED; }
    }
step_1:
    {
        GeasValue t1[1] = { f->slots[0] };
        GeasValue t2 = {0};
        GeasStatus t3 = geas_course_call(ctx, f, 0, t1, 1, &t2);
        if (t3 != GEAS_OK) { f->status = t3; return GEAS_COURSE_STOPPED; }
        f->slots[1] = t2;
    }
    /* ... */
    return GEAS_COURSE_DONE;
}
```

The emitter keeps these rules, and a handwritten course keeps them too:

- `f->step` is stored, and the checkpoint called, before the label of every suspending operation, and no two operations share a step number. A resumed course enters at the new step with the previous step's result already in its slots.
- Argument and arm arrays are built after the label, from slots, since a re-entry jumps straight to the label. No variable length array is declared anywhere in the function, and no `goto` jumps over an initialization.
- After an operation answers anything but `GEAS_OK`, the code touches no slot and does not read `frame->slots`, since a break during a call may have reclaimed the heap and left `slots` NULL. It sets `frame->status` and returns `GEAS_COURSE_STOPPED`.
- A polled operation is called again, with the same arguments, every time the runtime enters at its step, and the ready check compares against the constant, `if (ready != GEAS_COURSE_READY) return GEAS_COURSE_SUSPENDED;`.
- A `?` that meets an `Err` or a `None` returns `GEAS_COURSE_DONE`, and so does `return` and falling off the end.
- The frame pointer handed to an operation is the one the runtime handed the function.

**The operations.** Every operation takes `ctx`, the instance, and the frame. Called from any thread but the one running the course, from a pledge or remedy body, or on an instance with no course, each answers `GEAS_ERR_STATE`. Each begins the same way: it waits out an event delivery another thread holds on the instance, then runs the tick, so a deadline that passed lands before it looks, it answers a held autopark failure, and it refuses with `GEAS_ERR_STATE` when the contract reached Remedying or Broken or a break was requested, which ends the course CANCELLED whatever the function returns after it.

```c
GeasStatus geas_course_call(void* ctx, GeasCourseFrame* f, uint32_t pledge,
                            const GeasValue* args, size_t nargs, GeasValue* out);
GeasStatus geas_course_await(void* ctx, GeasCourseFrame* f, uint32_t signal,
                             int has_within, int64_t within_ms,
                             uint32_t* ready, GeasValue* out);
GeasStatus geas_course_sleep(void* ctx, GeasCourseFrame* f, int64_t duration_ms,
                             uint32_t* ready);
GeasStatus geas_course_select(void* ctx, GeasCourseFrame* f, const GeasCourseArm* arms,
                              size_t narms, uint32_t* ready, uint32_t* chosen,
                              GeasValue* payload);
GeasStatus geas_course_together(void* ctx, GeasCourseFrame* f, const GeasCourseCall* calls,
                                size_t ncalls, uint32_t* ready, GeasValue* outs);
int64_t    geas_course_now(void* ctx);
GeasStatus geas_course_checkpoint(void* ctx, GeasCourseFrame* f);
```

A select names its arms, and a together its calls, through two small structs, 16 and 24 bytes on LP64:

```c
typedef struct GeasCourseArm {
    uint32_t is_timeout;   /* 1 for a timeout arm, 0 for an await arm */
    uint32_t signal;       /* an await arm's signal, by pledges table index */
    int64_t  timeout_ms;   /* a timeout arm's duration from the select's first poll */
} GeasCourseArm;

typedef struct GeasCourseCall {
    uint32_t         pledge;   /* the pledge, by pledges table index */
    const GeasValue* args;     /* copied on the poll that starts the call */
    size_t           nargs;
} GeasCourseCall;
```

A call's arguments are copied onto the instance on the poll that starts it, so the array needs to live only through that poll, and the emitter builds both arrays after the step's label from slots.

- `geas_course_call` runs one of the contract's own pledges, by pledges table index, inline on the course's thread through the machinery a host fulfillment uses: the gate, the terms, the retries, the latch, the policy, the events, and the remedy pass a break makes due. `out` receives the pledge's value, an `Err` as readable as an `Ok`; any other status is the fulfillment's, and the course stops on it. A signal index is `GEAS_ERR_TYPE`, an index past the table `GEAS_ERR_NAME`, a count mismatch `GEAS_ERR_TYPE`.
- `geas_course_await` waits for a signal by its pledges table index. With `has_within` 0 it is ready once the signal is delivered and `out` is the raw payload; with `has_within` 1 it is ready once the signal is delivered or `within_ms` has passed, and `out` is an `Option`, tag 1 `Some` with the payload boxed on the instance, tag 0 `None`. A signal delivered before the await ran answers at once. A signal whose own `within` lapsed yields `None` at once with `has_within` 1, and is `GEAS_ERR_STATE` with `has_within` 0. A signal an amends block seeded fulfilled latched with no payload behind it: one with no parameters answers Unit, `Some(Unit)` with `has_within` 1, and one with parameters answers `GEAS_ERR_TYPE`, which halts the course. The compiler refuses that seed, so only a handwritten amends table meets it, and `select` answers the same way for an await arm on such a signal.
- `geas_course_sleep` is ready once `duration_ms` has passed since the step's first poll.
- `geas_course_select` is ready once any arm is, `*chosen` the first ready arm in arm order, so the emitter lays the arms out in source order. `payload` is the raw payload for an await arm and `Unit` for a timeout arm. A select whose every arm awaits a lapsed signal is `GEAS_ERR_STATE`.
- `geas_course_together` is one step of concurrent calls. Its first poll copies every call's arguments onto the instance, starts each call on the pool, and answers `*ready` `GEAS_COURSE_WAIT`. Once every call has an outcome the course is entered again at the same step, and that poll is ready with `outs[i]` the i-th call's value. A call whose status was not `GEAS_OK` makes the poll answer the first such status in call order, and the outcomes the other calls latched stand. A call whose `after` is not met is `GEAS_ERR_STATE` without running. Each poll at the step names the same pledges in the same order. An empty or NULL `calls`, a NULL `outs`, a signal index, or a count mismatch is `GEAS_ERR_TYPE`.
- `geas_course_now` reads the runtime clock, 0 for a NULL ctx.
- `geas_course_checkpoint` is called after every step advance. It publishes the frame's step to `geas_course_info` and `geas_course_wait` at once, under the course's own mutex, so a host reads the step a running course stands at. With autopark armed it then writes the park row for the frame as it stands and answers the write's status, and the course stops on a failure; without autopark it answers `GEAS_OK`.

A wait's deadline is fixed on the first poll of its step: the runtime stores it in `wait_due_ms` with `wait_step`, a later poll of the same step reads it back, and an operation that answers ready clears both, so a loop that comes back to the same step gets a fresh deadline. The earliest deadline the frame stores is 1, since 0 means none. Every wait that keeps a deadline ends once it is past, so a deadline an entry finds already past and still untouched when the function answers `GEAS_COURSE_SUSPENDED` belongs to no wait the course stands at, a row brought back with a stray one for instance; the entry clears `wait_due_ms` and `wait_step` and the run ends there, rather than entering the course again for it.

**The run.** Sign schedules the first entry. A pool worker takes the instance lock once and calls `course_fn` with it held, so the function's slot writes, its calls, and every operation it polls see one consistent instance. A call adds no hold of its own, which is what lets a retry delay and the gaps between remedies release the lock for real. When the function answers `GEAS_COURSE_SUSPENDED` the course reads WAITING and the worker goes back to the pool. A signal's first delivery, the lapse of a signal, a tick that finds the frame's deadline due, or the last call of a together finishing schedules the next entry. Only one thread is ever inside a course: an entry is scheduled at most once at a time, and a reason to enter that arrives while the function runs is kept and honored before the course is let go.

`DONE` finishes the course and `STOPPED` halts it on the frame's status. The course is cancelled instead when the contract reaches Remedying or Broken, when a break is requested, or when an operation was refused for one of those reasons: a waiting course is dropped the moment the state moves, and a running one is cut off at its next operation. None of these ends sets the contract's state, the requirements decide it from the latches. A course whose own step answered `Err` and broke the contract gets that `Err` back, returns `DONE` at its `?`, and reads FINISHED while the contract reads Remedying and then Broken.

**Locks and threads.** `course_fn` runs with the instance lock held by the pool worker. A host break waits on that lock, so it waits for the step the course is inside to return and latch, the same wait a break makes for a running host fulfillment, and the course is then cancelled at its next operation. The events a run records wait on the instance unreserved until the run lets the instance go, at its end or at a retry delay inside a call, and are delivered there, so a break or a signal from another thread while the course runs waits on the lock instead of finding the instance reserved for the whole run; a host mutation that lands while that delivery is under way waits it out as on any reserved instance, see Events, and `geas_course_wait` returning means the delivery has ended.

A delivery never stops a course. Another thread's delivery holds the instance reserved while its hook runs, a signal's caller, the ticker, or a worker, and the course meets it only where the run let the lock go. Wherever it does, at an entry, at an operation, at a call's gate or the end of its retry delay, at a checkpoint, and before an armed autopark writes a landing, the course waits for the delivery to end and goes on with the instance as it stands then, never answering `GEAS_ERR_STATE` for it: an entry defers to the redrive that ends the delivery, and a running course waits on a condition with the instance lock held exactly once, which the wait gives up whole, so the delivering thread takes the lock back to finish. A together call waits the same way before it starts, when its body returns, and after a retry delay. The waiting thread holds no reservation of its own, so the hook that delivers reads the instance freely, every read surface and `geas_course_info` included, while its mutations are refused as on any reserved instance. A break that lands during the wait is the cut, and the course ends CANCELLED at its next operation. A hook must not wait for the course, nor for work on the pool, since the waiting course keeps its worker.

The calls of a together run on other pool workers with the instance lock released, bounded by the pool size, so two bodies really run at the same time, and the course is no caller of the operations while they do. A pledge of a transactional subcontract keeps the lock across its body, since its episode rides the instance's open transaction. A deadline is applied when no body is in flight on the instance, the same rule a host driven body gets, so a term that runs out while a together's calls run is applied by the first tick after the last of them returns. A host break while the calls run returns with the contract Remedying before they finish; the running bodies finish and latch, calls not yet started never start, and the last body to return runs the remedy pass over everything that latched.

**The host surface.**

```c
typedef enum GeasCourseState {
    GEAS_COURSE_NONE = 0, GEAS_COURSE_RUNNING, GEAS_COURSE_WAITING,
    GEAS_COURSE_FINISHED, GEAS_COURSE_HALTED, GEAS_COURSE_CANCELLED
} GeasCourseState;

typedef struct GeasCourseInfo {
    uint32_t size;          /* sizeof(GeasCourseInfo), set by the caller */
    uint32_t state;
    uint32_t step;
    uint32_t status;
    int64_t  wait_due_ms;
} GeasCourseInfo;

GeasStatus geas_course_info(GeasContract* c, GeasCourseInfo* out);
GeasStatus geas_course_wait(GeasContract* c, int64_t timeout_ms, GeasCourseInfo* out);
GeasStatus geas_contract_set_autopark(GeasContract* c, const char* dsn, const char* key);
```

NONE is a contract with no course. RUNNING covers a course scheduled on the pool, running, or delivering the events of its last entry. `step` is the step the frame stands at, published at every checkpoint, so it reads the running step while the course runs and the step it rests or ended at afterwards. WAITING is suspended at a wait. FINISHED returned `DONE`, HALTED stopped on the status in `status`, and CANCELLED was cut off by a break or the contract breaking. The runtime fills only the fields that fit the caller's `size` and leaves `size` as it was; a size too small to hold `state` is `GEAS_ERR_TYPE`.

`geas_course_info` reads a published copy without waiting on the instance lock, running the tick first when the lock is free, so a host can poll it while a step runs. Both calls count themselves as readers of the course while they read, and `geas_contract_free` answers `GEAS_ERR_STATE` until the last reader is out, so a free racing a poll never pulls the course out from under it. `geas_course_wait` blocks until the course is not RUNNING and answers `GEAS_OK` with `out` filled. `timeout_ms` is real time and a negative value waits forever; when it runs out the call answers `GEAS_OK` as well, with `out` read as it stands, `state` RUNNING and `step` the step the course runs, so the caller tells a timeout from a settled course by `state` alone. A caller holding an instance lock or delivering events, the course function, a body, or a hook, gets `GEAS_ERR_STATE` without waiting, with `out` read as it stands, and so does a body a `together` runs, which holds no lock while it runs but belongs to the very step the wait would be waiting out. A host thread that waits with no timeout must not hold a lock a hook takes, since the wait returns only after the run's events were delivered.

`geas_contract_set_autopark` names the store and key an instance parks itself into from now on; the runtime keeps copies of both and a NULL `dsn` clears them. The row is the one `geas_instance_park` writes, under its rules, and it is written at every course checkpoint, whenever the course waits or ends, and at the next release of the instance lock after every latch, lapse, contract state change, saved retry, and remedy start or end, and before every remedy body; it works on a host driven instance too. A moment the park rules refuse, a body running or a receipt not waited, defers the write to the next such point. A failed write is reported on stderr, and on a course instance the next operation answers it and the course halts, while a course about to wait halts instead. An instance ended by an explicit break writes nothing more, its last row stands. A NULL contract, or a `dsn` with a NULL key, is `GEAS_ERR_TYPE`.

**Parking and resuming a course.** A course that waits or ended parks, and a running one parks only inside a call's retry wait or remedy pass, the row then entering that call again on resume. The outcomes a together already has are recorded and its other calls run again. A slot the wire codec cannot carry, an instance handle, makes the park `GEAS_ERR_TYPE` and writes nothing; the checker keeps instance types out of a course, so only a handwritten course meets that. On resume a row with a frame needs the registered `course_hash` to match, else `GEAS_ERR_VERSION`, and a frame for a contract that has no course is `GEAS_ERR_VERSION` as well. A course that was waiting or running is entered again at its saved step with its slots, a sleep or a `within` keeping its absolute due time; one that finished, halted, or was cancelled is not run again and reads as it was. A course contract whose row has no frame, one from before the frame existed, starts its course at step 0. Resume emits `RESUMED` then `COURSE_RESUMED` and replays no earlier event.

**Events.** A course adds six kinds, each with a NULL `item` and both states the contract's current one: `COURSE_STARTED` 15 at the first entry, `COURSE_SUSPENDED` 16 when an entry ends waiting, `COURSE_RESUMED` 17 at every later entry and at a resume, `COURSE_FINISHED` 18, `COURSE_HALTED` 19, and `COURSE_CANCELLED` 20. A course that runs to a wait reads, around its steps' own latch events, `COURSE_STARTED`, the latches, `COURSE_SUSPENDED`, and on the next entry `COURSE_RESUMED`.

## Events

A runtime can publish the transitions of its instances to one host callback. An operator console and a log can share that callback and read the instance there, without polling for changes.

```c
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
    GEAS_EVENT_LOST = 14,
    GEAS_EVENT_COURSE_STARTED = 15,
    GEAS_EVENT_COURSE_SUSPENDED = 16,
    GEAS_EVENT_COURSE_RESUMED = 17,
    GEAS_EVENT_COURSE_FINISHED = 18,
    GEAS_EVENT_COURSE_HALTED = 19,
    GEAS_EVENT_COURSE_CANCELLED = 20,
    GEAS_EVENT_AMENDED = 21,
    GEAS_EVENT_RELEASED = 22
} GeasEventKind;

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
GeasStatus geas_runtime_set_event_hook(GeasRuntime* rt, GeasEventFn fn, void* user);
```

The kind numbers and the prefix through `seq` are permanent. `size` is the producer's `sizeof(GeasEvent)`, 56 bytes on LP64. Future fields append after `seq`, so an older host continues reading its prefix and a newer host checks `size` before reading a later field. The existing public layouts stay as they were.

`instance` identifies the live handle and `contract` is its descriptor name. `item` is the pledge, signal, or remedy name, and NULL for a contract event. `old_state` and `new_state` are contract states, except that `LOST.new_state` counts dropped events, equal for other events that describe an item or an operation; `STATE_CHANGED` records a distinct pair. `at_ms` is the runtime clock when the transition is recorded, including a deadline applied by a later tick. `seq` starts at 1 and counts delivered events of this instance, including `LOST`, with no gaps across allocation failures, hook replacements, or intervals with the hook cleared. Sequence numbers are assigned during delivery, and `LOST` is the only sign that an enabled event site dropped an event. Changes made while no hook is installed are unobserved and never replayed. A resumed handle starts its own sequence at 1.

**Delivery.** Every queued event is delivered exactly once in its instance's sequence order, on the thread that recorded it. A park taken by another thread during a remedy wait leaves its event with the pass owner, who delivers it at the next delivery boundary with its captured callback pair. A pledge outcome runs on its worker, a signal on its caller, a deadline on the thread applying the tick, and a remedy event on the thread running the pass. Different instances may call the hook concurrently. The runtime captures events under the instance lock and drains a queue after that thread releases every instance lock, including the caller locks around nested contract calls. No instance, runtime, future, or queue mutex is held across the hook. Retry delays and the intervals between remedies provide a delivery boundary when they release the last instance lock.

The instance stays reserved until that queue drains. An observed remedy pass keeps its reservation across retry delays until the pass ends. Another thread can park during that wait or between remedies once the previous event batch has drained; the snapshot leaves the reservation and the live pass with their owner. State, partial results, attrs, vow reads, remedy reads, lapse reads, and signal payload reads are allowed, from the hook and from other threads, and they do not tick or run remedies during delivery. They see a consistent state at the delivery boundary. Several transitions may have completed since an event was recorded, so the event's state pair describes its transition and a state read describes the settled boundary. An explicit reclaim can therefore leave an earlier event's payload unavailable by delivery, following the ordinary payload lifetime rule.

A host call waits out another thread's delivery. A host thread here is one outside every body, hook, delivery, ref write back callback, together body, and remedy pass, holding no instance lock. Its `geas_pledge_fulfill` and `geas_pledge_fulfill_sync`, `geas_signal`, `geas_instance_park` and `geas_instance_park_v`, `geas_contract_break`, `geas_contract_release`, `geas_contract_set_autopark`, `geas_contract_free`, and `geas_instance_tick` take the instance lock, wait on a condition with that lock given up until the delivery ends, and then run against the instance as it stands, so each answers what it would answer with no hook installed. The pool worker running a queued fulfillment waits the same way at its gate and when it wakes from a retry delay, so a host driven pledge whose retry delay ends inside another thread's delivery makes its next attempt once that delivery ended. A fulfill, a signal, or a tick that meets a remedy pass another thread owns does not wait for the pass: the contract is Remedying and the call answers as it does on any Remedying contract. A break, a release, and a free wait for the pass to end, the way break waits for a pass anyway, and a park waits only while the delivery refuses it, so a snapshot during a remedy wait or between remedies goes ahead. A waiter that wakes to an instance a break or a release reclaimed meanwhile answers `GEAS_ERR_STATE`, as every call on a reclaimed instance does. `geas_contract_request_cancel` never waits, and neither does any read.

The calls made from inside the delivery stay refused. Fulfill, signal, park, break, and release from a hook on the instance it delivers return `GEAS_ERR_STATE` at once, nothing latched and nothing published, and so do the same calls from a body, a write back callback, or a together body on an instance another thread is delivering, since the delivering thread may be waiting on them; the rules a body already keeps for its own instance stand as they are. The future form of fulfill reports that status through wait as usual. A break whose request was published just before the delivery took its reservation is carried out by the delivery once its callbacks returned, and a break caller waiting meanwhile returns once it has, with that break's status. When a remedy pass is under way on another thread at that moment, the delivery hands the break to that pass along with the reservation and every event queued in the meantime, and the pass delivers those events at its own outermost release of the instance lock and carries out the break when it ends, so the heap is reclaimed exactly once, by the pass. The producing thread may finish its operation and nested calls before delivery begins. `geas_runtime_tick` and the ticker pass over an instance another thread is delivering and the next tick reaches it, since a hook may stop the ticker, which joins it, and the ticker must never be waiting on that hook. A course's own call, and a together call, waits that delivery out whole, a remedy pass's reservation included, see The course. A hook uses the read surface, returns normally, and does not shut down its runtime or wait for work it has prevented from proceeding.

**Host locks.** Waiting makes the host's own lock order part of the bargain: a host thread does not make a waiting call, and does not wait on a receipt, while it holds a lock its hook takes. The hook cannot return until it gets that lock and the call cannot return until the hook does, so the two wait on each other for good; that is the host's lock order to keep, the runtime does not detect it. Every read, `geas_future_poll`, `geas_course_info`, hook registration, and `geas_contract_request_cancel` never wait on a delivery, so a host may make those under such a lock, and a hook may take a host mutex while another thread holding it reads the instance. A hook that starts work on another instance and waits for it needs a pool worker while every worker may be waiting out a delivery, which is one more reason a hook keeps to the read surface.

**The order a host sees.** A pool worker publishes a fulfillment's outcome after it delivered the events that fulfillment recorded. So when `geas_future_wait` or `geas_pledge_fulfill_sync` returns, every event the call's own fulfillment recorded has reached the hook, a single threaded host sees a call's events before the call returns, and its next call never meets that delivery. Events handed to a remedy pass another thread owns are delivered by that pass, at its own outermost release of the instance lock. A break that reclaims the instance between that delivery and the publication forfeits the receipt like any unwaited one, so a host that wants the value waits before it breaks. A body that breaks or releases its own instance forfeits its own receipt to `GEAS_ERR_STATE` too, and its worker publishes that receipt in the same order, once the RECLAIMED or RELEASED event the ending recorded is delivered, so a wait on it that returns has seen that event. A hook that waits on the receipt whose events it is delivering is refused with `GEAS_ERR_STATE` instead of waiting on its own thread; a hook that waits on any other receipt whose fulfillment is waiting out this same delivery waits for good, which the hook rule above already forbids. Retry delays and the intervals between remedies can deliver earlier batches while a future is still pending. Wait and poll describe the fulfillment and the events it recorded on its own worker, not the deliveries of other threads, and runtime shutdown, which joins the producing workers, is the final delivery fence. Hooks can provide their own notification when a host needs to follow their progress.

**Registration.** A NULL runtime is `GEAS_ERR_TYPE`. Otherwise setting, replacing, or clearing the hook returns `GEAS_OK` before or after freeze. A NULL `fn` clears it. The function and user pointer are captured together at each transition. Registration never waits for callbacks; events already captured keep their original pair after replacement or clear. The host keeps those function and user pointers alive until their delivery threads finish, including the pass owner for a park during a remedy wait, or simply until runtime shutdown. The `GeasEvent*` is borrowed only during the callback; its names and instance handle live until shutdown. A disabled event emission performs one atomic pointer check, with no queue allocation, clock read, or registration lock. An allocation failure increments a counter on that instance and leaves the host running. The next successfully delivered batch begins with one `GEAS_EVENT_LOST`, stored inside the instance so it needs no allocation. Its `item` is NULL, its `new_state` is the number dropped since the previous marker, saturating at `UINT32_MAX`, and its other fields and callback pair come from the first queued event of that batch. A batch with no queued entries leaves the counter for the next batch. A cleared hook does not count unobserved sites as losses.

**The sequence.** Sign emits `STATE_CHANGED`, Unsigned to Signed. A pledge's first Ok emits `LATCH_FULFILLED`, its first Err emits `LATCH_BROKEN`, and a deadline emits `LATCH_LAPSED` instead of a second broken event. A subcontract deadline emits one lapse for each still pending member in declaration order. A signal's first delivery emits `LATCH_FULFILLED` followed by `SIGNAL_DELIVERED`. Repeated outcomes and duplicate signals emit nothing when no latch or state moves. Policy evaluation follows the item events and emits `STATE_CHANGED` only when the contract state changes.

A contract term emits `TERM_LAPSED` before its state transition. A break with remedies enters Remedying, then each remedy emits `REMEDY_STARTED` and one `REMEDY_DONE` or `REMEDY_FAILED`, newest fulfilled pledge first, and the pass emits the transition to Broken. Retry attempts have no separate event. Open transactions roll back before the remedy pass and the delivery boundary, so the callback sees their resolved fate.

Explicit cancel emits `CANCELLED` once when the request enters the locked lifecycle, followed by the transitions, remedies, and `RECLAIMED` it causes. A request seen by a running body is recorded on that body's thread when the runtime carries it into the lifecycle; a deferred break inside a body is recorded there and its reclaim follows the outermost body. A cancel accepted just before another thread reserves delivery returns without waiting for that callback; the producing thread carries it into the lifecycle after the captured queue drains. If a deferred cancellation fails after callbacks, a break call still waiting receives that status; with no break call waiting, the runtime writes the status to stderr with its `[libgeasrt]` prefix. Delivery continues. Repeated break calls emit no second cancellation or reclaim. An automatic break keeps the heap and emits no reclaim. A successful park emits `PARKED` after its row is written and leaves the live instance's state unchanged. A successful resume emits only `RESUMED` for the restored state, without replaying latches or remedy history; a resume through an amends block emits `AMENDED` 21 just before it, `old_state` and `new_state` the two versions, see Amendment. Its later tick applies elapsed deadlines and unfinished remedies normally. A release emits `RELEASED` 22 once where an explicit break would emit `RECLAIMED`, `item` NULL and both states the one the instance was released in, with no `CANCELLED`, no remedy event, and no `STATE_CHANGED` before it, see Handing an instance off; a cancel request emits nothing. A claim emits what a resume emits. Shutdown releases storage without synthesizing contract transitions.

Python exposes `rt.on_event(callback)`, or `rt.on_event(None)` to clear. The callback receives an `Event` with `kind`, `instance`, `contract`, `item`, `old_state`, `new_state`, `at_ms`, and `seq`. Names and scalar fields are copied into Python; `instance` is a borrowed `Contract` handle with the read rules above. The runtime wrapper anchors replaced callbacks until shutdown. A Python callback exception prints its traceback and delivery continues.

## Cancel

`geas_contract_break` is cancel. It publishes a request before waiting for the instance lock, so a body running on another thread can see the request while the breaker waits. The body is never interrupted. Break waits for it to return, latches its outcome as usual, runs the remedies when the contract was Signed or Partial, then reclaims the instance heap and returns. A body that finishes with Ok after the request gets its remedy, including when that Ok would otherwise fulfill the whole contract; a body whose first outcome is Err gets none. Queued work does not start once cancel is requested.

```c
int geas_cancel_requested(void* ctx);
```

A body passes the `ctx` it was handed. The call reads atomic flags and takes no instance lock: it answers 1 once a break, a cancel request, or a release is called on that instance, including a break or a release deferred from inside a body, or when runtime shutdown starts, and 0 otherwise. The request stays set for the rest of that instance's life. A remedy is the exception: on the thread running an instance's remedy pass the call answers 0 for that instance whatever was requested, since the break that made the remedies due is the request, and a remedy that stopped for it would leave undone the very work the break asked for; only runtime shutdown reads 1 there. A child process a remedy runs through `Process.run` polls the same way, so a break never cuts a compensation short. A fresh sign or resume starts with no request, cancellation is not part of the park row. In Python a bound callback receives the instance and calls `inst.cancel_requested()`.

A break called from inside a body on its own instance returns `GEAS_OK` immediately and leaves the break pending until the outermost fulfillment ends. Its outcome latches before the remedies run, and the heap remains available until then. A break while another thread runs the remedy pass waits for that pass. Cancellation does not skip remedies or shorten their retry terms.

```c
GeasStatus geas_contract_request_cancel(GeasContract* c);
```

So break is the request and the ending in one call, and `geas_contract_request_cancel` is the request on its own. It raises the same flag and returns at once, it waits on no lock, runs no remedy, and breaks nothing. Every body on the instance reads `geas_cancel_requested` as 1 from then on, a default retry wait wakes and its fulfillment answers `GEAS_ERR_STATE` without another attempt, and a course is cut at its next operation and ends `CANCELLED`, its contract left in whatever state the latches gave it. Now the instance stays live: a body that returns after the request latches its outcome as usual, and later fulfillments and signals are taken, their bodies reading the flag the same way. Nothing lowers the flag, it stays raised until the instance ends in a break or a release, so a host that asked once never has to ask again and a body never sees a request come and go. A second request is the same request and answers `GEAS_OK`, a request after a break or a release reclaimed the instance answers `GEAS_ERR_STATE`, and a NULL handle `GEAS_ERR_TYPE`. A host with calls in flight on several threads requests the cancel, waits for its own calls to come back, and then breaks or releases, so no thread is left waiting on another. In Python it is `inst.request_cancel()`, in dusk `contract_request_cancel(c)`.

Under a request, an attempt's outcome is read for whether it would be retried. An outcome the pledge's `retry` would try again, a status other than `GEAS_OK` or an `Err` whose variant `on` lists, with retries still left, is not latched: the fulfillment answers `GEAS_ERR_STATE`, its value zeroed and the pledge still pending, since an outcome the pledge never meant to keep is not one a cancel should make final. An outcome no retry would follow, an `Ok`, an `Err` no `on` lists, or the last attempt's answer, latches as usual. A break, a release, or shutdown hands every attempt's outcome back as it is, a retryable `Err` latching. Cancellation wakes a pledge's default retry delay and the fulfillment answers `GEAS_ERR_STATE` without another attempt. With a host supplied sleep callback, the receipt is forfeited immediately, and the worker makes no further attempt when the callback returns. The runtime never interrupts that callback; a scheduler that wants its worker back promptly wakes its own wait, or polls cancellation through its user context. Shutdown publishes its flag before joining workers, so bodies and sleep callbacks can finish cooperatively.

## Handing an instance off

So a park writes a row and leaves the instance live where it is, which is what a checkpoint wants, however a hand off wants the opposite: one process lets the obligation go, another one picks it up, and at no point do both of them hold it or does either one run a remedy for work the other is about to finish. Three calls make that move, in this order.

```c
GeasStatus geas_instance_park(GeasContract* c, const char* dsn, const char* key);
GeasStatus geas_contract_release(GeasContract* c);
GeasStatus geas_instance_claim(GeasRuntime* rt, const char* dsn, const char* key,
                               uint64_t expected_hash, GeasContract** out);
```

**Park**, or an armed autopark, puts the row in the store, the same row a resume reads.

**Release** lets the instance go from this process with no remedy and no state change. It publishes a cancel before it waits for the instance lock, so a body that polls `geas_cancel_requested` returns, and it waits out a body running on the instance and another thread's remedy pass, the way break does. Then a running or waiting course is cut, an open transactional episode rolls back and frees its store locks, every unwaited future is forfeited to `GEAS_ERR_STATE`, and the heap is reclaimed exactly as an explicit break reclaims it. The contract state is left as it was, never Remedying or Broken, and the call emits `RELEASED`. Every later call on the handle answers as on a broken one, `GEAS_ERR_STATE`, a fulfillment, a signal, a park, a break, a request, and a second release included, while the state and the partial reads keep answering what the instance was. A release of an Unsigned instance, of one a break already reclaimed, or of one with a break pending is `GEAS_ERR_STATE`, and a NULL handle is `GEAS_ERR_TYPE`.

An instance armed with autopark that has changes not yet written writes its last row first, so the row is never older than what this process finished. That row records an open episode as rolled back and a course the release cut as still running at its step with no status, so the claimer carries the course on from there. A write that fails is the call's status, and the instance goes on as it was, live and unreleased, for the host to retry or to break. An instance with no autopark writes nothing at release: the host parked it, and a latch that moved after that park is the host's to have parked again before releasing, the runtime cannot tell which row the host means to hand over.

Called from inside a body on its own instance, the release is noted and answers `GEAS_OK` at once, and it happens when that body's fulfillment ends, the same deferral break follows. Called from a host thread while another thread delivers the instance's events, it waits that delivery out first, see Events; from a hook on the instance it is delivering it is `GEAS_ERR_STATE`. The body's outcome latches first, and a fulfillment the release ended answers `GEAS_ERR_STATE` through its wait. A break called while a release is pending answers `GEAS_ERR_STATE`. A deferred release that then fails, its last autopark row refused say, goes back only to a release caller: a `geas_contract_release` still inside its call takes the status as its answer, and with none in its call the runtime writes the status to stderr under its `[libgeasrt]` prefix. No break, fulfillment, or other call is handed that status, no `RELEASED` is emitted for it, and the instance goes on live and unreleased.

**Claim** resumes the row and takes it out of the store in the same move. The row is read and the instance built from it unpublished, exactly the way `geas_instance_resume` builds one, amends blocks and a course frame included, and a slot in the runtime's instance table is held for it. Then one `DELETE` that matches the row on every column it was built from, its `gen` included, takes it, as its own transaction, and it took the row exactly when it reports one row deleted. Only when that delete took the row does the instance publish, emit `RESUMED` (after `AMENDED` for an amended row), and start its course, so no tick, no ticker, and no course step reaches the instance before the row is gone. Two claimers of one key, in one process or in two, both read and both build, exactly one delete takes the row, and that claimer gets the instance while the other's built instance is discarded and its call answers `GEAS_ERR_NAME`, the row being gone. A row rewritten between the read and the delete, by an autopark still running in the releasing process for instance, is read and built again, and a key rewritten eight times in a row answers `GEAS_ERR_STATE`. A build that fails, `GEAS_ERR_VERSION`, `GEAS_ERR_TYPE`, `GEAS_ERR_UNBOUND`, an amends block's own status, or `GEAS_ERR_OOM` when the instance table cannot grow, leaves the row where it was and answers that status, all or nothing. Every other status follows `geas_instance_resume`, and `geas_instance_resume` itself never deletes a row.

The build happens before the delete, not inside one store transaction with it, because the contract a row holds can name the same store in its own `dsn` vow, and a build that opened that store while the claim held a write lock on it would wait on itself. The conditional delete gives the same answer the transaction would: the row a claimer took is the row it built.

Put together, the order is park, release, claim, and the remedies run nowhere on the way. A host that releases without having parked, with no autopark armed, has let the obligation go with no row to take it up from, which is the host's call to make and not one the runtime second guesses. In Python the three are `inst.park(dsn, key)`, `inst.release()`, and `rt.claim(dsn, key, expected_hash=0)`, the event kind `GEAS_EVENT_RELEASED`; in dusk they are `contract_park`, `contract_release(c)`, and `contract_claim(rt, dsn, key, hash)`.

## The store calls

So the `Store` operations of `std.store` lower onto one family of runtime calls, and a host bound body may call them the same way compiled code does. Each takes the instance a body runs against and a schema of that instance's own contract, binds every value as a positional parameter so a string is a string and never SQL, and lands its answer in `out` as a `Result` owned by the instance, its `Ok` arm built by the call:

```c
typedef enum GeasStoreCmp {
    GEAS_CMP_EQ = 0, GEAS_CMP_NE = 1, GEAS_CMP_LT = 2,
    GEAS_CMP_LE = 3, GEAS_CMP_GT = 4, GEAS_CMP_GE = 5,
} GeasStoreCmp;

typedef struct GeasStoreTerm {
    uint32_t col;              /* index into the schema's columns */
    uint32_t cmp;              /* a GeasStoreCmp */
    const GeasValue* value;    /* compared against the column, always bound */
} GeasStoreTerm;

typedef enum GeasStoreAgg {
    GEAS_AGG_MIN = 0, GEAS_AGG_MAX = 1, GEAS_AGG_AVG = 2,
} GeasStoreAgg;

#define GEAS_STORE_NO_ORDER UINT32_MAX

GeasStatus geas_store_find(GeasContract* c, const GeasSchemaDesc* schema,
                           const GeasValue* key, GeasValue* out);
GeasStatus geas_store_query_eq(GeasContract* c, const GeasSchemaDesc* schema,
                               uint32_t col, const GeasValue* value, GeasValue* out);
GeasStatus geas_store_query_where(GeasContract* c, const GeasSchemaDesc* schema,
                                  const GeasStoreTerm* terms, uint32_t nterms,
                                  GeasValue* out);
GeasStatus geas_store_query_ordered(GeasContract* c, const GeasSchemaDesc* schema,
                                    const GeasStoreTerm* terms, uint32_t nterms,
                                    uint32_t order_col, uint32_t order_desc,
                                    GeasValue* out);
GeasStatus geas_store_query_page(GeasContract* c, const GeasSchemaDesc* schema,
                                 const GeasStoreTerm* terms, uint32_t nterms,
                                 uint32_t order_col, uint32_t order_desc,
                                 const GeasValue* limit, GeasValue* out);
GeasStatus geas_store_count_where(GeasContract* c, const GeasSchemaDesc* schema,
                                  const GeasStoreTerm* terms, uint32_t nterms,
                                  GeasValue* out);
GeasStatus geas_store_sum_where(GeasContract* c, const GeasSchemaDesc* schema,
                                uint32_t sum_col, const GeasStoreTerm* terms,
                                uint32_t nterms, GeasValue* out);
GeasStatus geas_store_select(GeasContract* c, const GeasSchemaDesc* schema,
                             const GeasStoreTerm* terms,
                             const uint32_t* group_lens, uint32_t ngroups,
                             uint32_t order_col, uint32_t order_desc,
                             const GeasValue* limit, GeasValue* out);
GeasStatus geas_store_count_any(GeasContract* c, const GeasSchemaDesc* schema,
                                const GeasStoreTerm* terms,
                                const uint32_t* group_lens, uint32_t ngroups,
                                GeasValue* out);
GeasStatus geas_store_sum_any(GeasContract* c, const GeasSchemaDesc* schema,
                              uint32_t sum_col, const GeasStoreTerm* terms,
                              const uint32_t* group_lens, uint32_t ngroups,
                              GeasValue* out);
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
```

The answers: `find` lands `Ok(Some(row))` or `Ok(None)` by primary key, the first column; `query_eq`, `query_where`, `query_ordered`, `query_page`, and `select` land `Ok` of a list of rows; `count_where` and `count_any` land `Ok` of an `Int`; `sum_where` and `sum_any` land `Ok` of the summed column's own type, `Int` or `Float`, its zero when nothing matches; `agg_any` lands `Ok` of an `Option`, the column's type for `GEAS_AGG_MIN` and `GEAS_AGG_MAX` and `Float` for `GEAS_AGG_AVG`, `None` when nothing matches; and `insert`, `update`, and `delete` land `Ok(Unit)`. A row is a record whose fields are the schema's columns in order. The terms of a `_where` call are ANDed in order. The `_any` calls and `select` split the terms into `ngroups` groups, `group_lens[g]` terms each, AND the terms of a group, and OR the groups. `order_desc` is 0 for ascending and 1 for descending, `select` takes `GEAS_STORE_NO_ORDER` for the backend's own order, and a `limit` is a nonnegative `Int` bound after every term, given to `select` only beside an order.

`schema` has to be a pointer into the instance's own descriptor, an entry of its `schemas` table, never a copy of one; any other pointer, and any schema on a contract that declares none, is `GEAS_ERR_TYPE` before anything runs, so a body cannot aim a write at a table its contract never vowed. A NULL argument is `GEAS_ERR_TYPE`, a store-backed instance whose connection a break or a release already closed is `GEAS_ERR_STORE`, a backend that cannot complete the operation is `GEAS_ERR_STORE`, and an allocation that fails is `GEAS_ERR_OOM`. The `Err` arm of every answer is the language's `StoreError` and no call here builds it; a failure is the status, never a value.

## Program steps

```c
GeasStatus geas_process_run(void* ctx, const GeasValue* args, size_t nargs,
                            GeasValue* out);
```

`geas_process_run` is the call `Process.run(command)` lowers onto, and it runs in the thunk frame of the body that calls it: `ctx` the instance, `args` one slot holding the `Command` record, `program`, `args`, `stdout_path`, and `stderr_path` in that order, and `out` a `Result` on the instance, `Ok(Unit)` for an exit of zero and `Err` of the `ExitError` sum otherwise, `Exited(code)` as variant 0, `Signaled(number)` as variant 1, and `SpawnFailed(reason)` as variant 2. A frame that is not one well formed `Command` is `GEAS_ERR_TYPE`. The child is launched with `posix_spawn`, and the wait polls `geas_cancel_requested` every 10 ms, sending SIGTERM and then SIGKILL 200 ms later when a cancel lands; the spec's Program steps section has the whole contract.

## Calls outside the ABI

`libgeasrt` exports two more families than the ones above, and neither one is part of the 1.x ABI, see Stability.

The store driver in `geas_store.h`, `geas_store_open`, `geas_store_close`, `geas_store_exec`, `geas_store_exec_params`, `geas_store_query`, `geas_store_reconcile`, `geas_store_begin`, `geas_store_commit`, and `geas_store_rollback`, is the connection layer the runtime's own store calls and its park table sit on: one connection to one database, statements with positionally bound parameters, and rows handed back as `GeasValue`s allocated through a caller's allocator. No emitted module calls it, and a host that drives a contract never needs it, since a store-backed contract opens, reconciles, and closes its own connection. It is exported so the runtime's gates can test the driver on its own, and its shape is the seam a second backend will plug into, so it may change in a minor release.

The frame calls in `geas_wire.h`, `geas_wire_frame_write` and `geas_wire_frame_read`, read and write a 20 byte frame header, the magic `ASHW`, a `u32` kind from 1 to 13, a `u64` request id, and a `u32` payload length of at most 64 MiB, all little endian. Nothing in the runtime, the compiler, or the bridge speaks those frames, the bridge rides gRPC, so they stay outside the ABI as well. The value codec in the same header, `geas_wire_encode_value` and `geas_wire_decode_value`, is the format the park row is written in, and that part is frozen, see the next section.

## The value wire codec

```c
GeasStatus geas_wire_encode_value(const GeasValue* v, uint8_t* buf, size_t cap,
                                  size_t* need);
GeasStatus geas_wire_decode_value(GeasContract* owner, const uint8_t* buf,
                                  size_t len, GeasValue* out, size_t* consumed);
```

So the value codec is the one serialization of a `GeasValue`, the bytes every value column of the park row is written in, and it is canonical: one value has one byte string, and encoding a decoded value reproduces its input exactly. `geas_wire_encode_value` follows the size protocol of `geas_iname_dump`, `*need` the exact encoded size, the bytes written only when `cap` holds them, `GEAS_ERR_OOM` and nothing written otherwise. `geas_wire_decode_value` builds the value through the allocation helpers on `owner`, so it is instance owned and dies at that instance's break, and reports through `*consumed`, when not NULL, the bytes the value took; bytes after a complete value are the caller's to read. On any error the decoder hands back a zeroed Unit and `*consumed` 0, and what it already copied onto `owner` stays there until the break.

**The layout.** Every integer is little endian at a fixed width and nothing is padded. Every value starts with an 8 byte head, `u32 ty` then `u32 tag`, its `GeasTypeTag` and its variant tag, and the body after it depends on the type:

| type | tag | body after the head |
|---|---|---|
| `GEAS_TY_UNIT` | 0 | nothing |
| `GEAS_TY_INT`, `GEAS_TY_UINT` | 0 | 8 bytes, the integer |
| `GEAS_TY_FLOAT` | 0 | 8 bytes, the IEEE 754 bits as they stand |
| `GEAS_TY_BOOL` | 0 | 1 byte, 0 or 1 |
| `GEAS_TY_BYTE` | 0 | 1 byte |
| `GEAS_TY_CHAR` | 0 | `u32`, a Unicode scalar value |
| `GEAS_TY_STRING`, `GEAS_TY_BYTES` | 0 | `u64` length, then that many bytes, no terminator |
| `GEAS_TY_LIST` | 0 | `u64` count, `u32` element tag, then each element |
| `GEAS_TY_MAP` | 0 | `u64` slot count, twice the pair count, `u32` key tag, then key and value interleaved |
| `GEAS_TY_TUPLE`, `GEAS_TY_RECORD` | 0 | `u64` count, then each field in order |
| `GEAS_TY_SUM` | the variant | `u64` count, then each payload field in order |
| `GEAS_TY_OPTION` | 0 for None, 1 for Some | nothing for None, the payload for Some |
| `GEAS_TY_RESULT` | 0 for Ok, 1 for Err | the payload, always |

The codec checks no UTF-8, a `String`'s bytes cross as they are, and the checked boundary is what holds a string to UTF-8 where one crosses into a body. An empty string or list still carries its length. A map key is a keyable scalar and the keys of one map are distinct. An `Ok` or `Err` of Unit is always written as a payload holding Unit, which is the one spelling the decoder hands back, a boxed Unit, so a value built with no box behind a Unit arm encodes to the same bytes.

**What the encoder refuses.** Nothing is written and `GEAS_ERR_TYPE` is answered for a value carrying `GEAS_TY_INSTANCE`, `GEAS_TY_PLEDGE_REF`, or a tag past the enum anywhere, a variant tag no value of its type carries, a `Bool` past 1, a `Char` that is not a scalar value, a `None` with a box, a length with no data behind it, a `List` or a `Map` longer than its capacity, a list's element tag or a map's key tag that is not a type, a key that is not keyable, and nesting past `GEAS_WIRE_MAX_DEPTH`, the boundary's own 256 levels, so a value a boundary accepted always encodes. A size past `SIZE_MAX` is `GEAS_ERR_OOM`.

**What the decoder refuses.** Nothing in an encoded value is trusted. Every length is checked against the bytes left before it is read, and every count has to fit in the bytes left at eight bytes an item, the smallest any value takes. The decoder also keeps one budget for the whole buffer, `len / 8` value slots, charged for every container's count and every box behind a `Some`, an `Ok`, or an `Err`, so a run of empty containers nested inside one another cannot claim more memory than the buffer could describe. Malformed bytes, a forbidden or unknown tag, a `Bool` past 1, a `Char` past U+10FFFF or in the surrogate range, a key that is not keyable or repeats, a count the bytes left cannot hold, an exhausted budget, and nesting past 256 levels are `GEAS_ERR_TYPE`; a count past `GEAS_VALUES_MAX_COUNT`, the most `GeasValue`s one allocation can hold, and an allocation that fails are `GEAS_ERR_OOM`. Bytes that decode are a well formed value, and that is all the decoder vouches for: a stored `Int` edited into a `UInt` decodes fine, so a resume and a claim hold what the row brought back to the contract's type table, see The checked boundary, and refuse it there.

The codec itself has no cap on the length of a buffer. It does hold the node budget, see The checked boundary: a value of more than `GEAS_VALUE_MAX_NODES` nodes, counted as the tree it unfolds to, is `GEAS_ERR_OOM` before any size is measured, with `*need` set to 0, a size no encoded value has, so a sizing call tells a value past the budget from a buffer too small, and a park that meets such a value answers `GEAS_ERR_OOM` and writes nothing. That is what keeps every row the runtime writes one a resume reads back, since the boundary a resume crosses holds the same budget. A deep copy follows at most 4096 levels.

## The park row

```c
GeasStatus geas_instance_park(GeasContract* c, const char* dsn, const char* key);
GeasStatus geas_instance_resume(GeasRuntime* rt, const char* dsn, const char* key,
                                uint64_t expected_hash, GeasContract** out);
GeasStatus geas_instance_park_v(GeasContract* c, const GeasValue* dsn,
                                const GeasValue* key);
GeasStatus geas_instance_resume_v(GeasRuntime* rt, const GeasValue* dsn,
                                  const GeasValue* key, uint64_t expected_hash,
                                  GeasContract** out);
```

`geas_instance_park` writes one row into the `geas_park` table and `geas_instance_resume` reads it back. The `_v` spellings take the dsn and the key as `String` values, the form a compiled pledge body holds, refuse any other tag with `GEAS_ERR_TYPE`, and otherwise follow the C string forms exactly.

The table is

```sql
CREATE TABLE IF NOT EXISTS geas_park (pkey TEXT PRIMARY KEY, contract TEXT,
    version INTEGER, shape_hash INTEGER, state INTEGER, signed_at INTEGER,
    vows BLOB, latches BLOB, errs BLOB, subtxn BLOB, flow BLOB, gen INTEGER)
```

and a park writes the whole row with `INSERT OR REPLACE`, so a key holds one row, the latest. The row has two generations and the runtime reads both. Format 1 is the first ten columns, `pkey` through `subtxn`; format 2 adds `flow`, one blob holding the workflow state, and `gen`. Each column holds:

| column | contents |
|---|---|
| `pkey` | the key, bound as its bytes |
| `contract` | the contract name, bound as its bytes |
| `version` | the contract's version, the `u32` |
| `shape_hash` | the shape hash, its 64 bits read as a signed integer |
| `state` | the `GeasContractState`, 1 through 5 |
| `signed_at` | the signing second, the clock's millisecond reading divided by 1000 |
| `vows` | every vow's wire value back to back in declaration order, no count, each one's type the vow's |
| `latches` | one byte per pledges table entry, 0 pending, 1 fulfilled, 2 broken |
| `errs` | per pledges table entry, a presence byte, then the `Err` payload's wire value when it is 1 |
| `subtxn` | one byte per subcontract, see Transactional fates |
| `flow` | the workflow blob below, empty for a format 1 row |
| `gen` | a nonzero odd number drawn at every write, `random() \| 1`, read as 0 on a row that has none |

`gen` is what makes a claim exact. A row parked twice with the same bytes still carries a fresh `gen`, so a claimer that read the older write matches no row with its `DELETE` and reads the row again, and a row written before the column existed matches on 0.

A table created before `flow` or `gen` existed is brought forward in place the first time this runtime parks into it, resumes from it, or claims from it, with one `ALTER TABLE geas_park ADD COLUMN flow BLOB` and one `ALTER TABLE geas_park ADD COLUMN gen INTEGER`, each issued only after `pragma_table_info` showed that column missing, and the rows already in it read an empty `flow`, which is how a format 1 row is recognized, and a `gen` of 0. Several processes may bring one table forward at once: another connection can add the column between the look and the `ALTER`, so an `ALTER` that fails is followed by a second look, and the column there now counts as success, while any other failure is the call's `GEAS_ERR_STORE`.

The blob is versioned on its own. The writer emits version 5, and the reader accepts versions 2 through 5. Version 2 has the same fields below except for `retry_attempts` and `retry_due_ms`; those resume as zero, with a full budget on the next fulfillment. Version 3 ends after the remedies, version 4 adds the course frame and the together section at the tail, and version 5 has version 4's bytes with the stamps read differently: a not yet eligible item is stamped `INT64_MIN`, the runtime's `GEAS_NOT_ELIGIBLE`, and every other stamp, a negative one included, is the clock reading it was. A version 4 or older blob spells not yet eligible as any negative stamp, and a negative `wait_due_ms` there is `GEAS_ERR_TYPE`. Format 1 has no blob, rather than a version 1 blob. Integers are little endian at fixed widths and values are the wire codec's canonical encoding, the bytes the `vows` and `errs` columns already use:

```text
"GFLW"                          4 bytes, magic
u32  version                    5
i64  signed_ms                  sign time on the runtime clock
u8   term_lapsed
u32  next_seq                   latches fulfilled so far
u32  npledges                   must equal the descriptor's
     per pledge:
       i64  eligible_ms         INT64_MIN while not yet eligible
       u8   lapsed
       u32  latch_seq           1 based order of the fulfilled latch, 0 if none
       u8   ok_present          then the wire value when 1
       u32  retry_attempts      attempts completed before the saved retry
       i64  retry_due_ms        absolute runtime clock time, 0 without a retry
u32  nsubs                      must equal the descriptor's
     per subcontract:
       i64  eligible_ms
u32  nremedies                  must equal the descriptor's
     per remedy:
       u8   state               GeasRemedyState
       u32  status              last attempt's GeasStatus
       u8   lapsed
       u8   err_present         then the wire value when 1
u8   has_course                 0 and nothing more for a host driven contract
     when 1:
       u64  course_hash         must equal the registered descriptor's
       u8   state               GeasCourseState, RUNNING through CANCELLED
       u32  step
       u32  status
       i64  wait_due_ms         absolute runtime clock time, 0 when nothing is due
       u32  wait_step
       u32  nslots              must equal the descriptor's course_nslots
       per slot:
         u8   present           always 1, then the wire value
       u32  join_step           the together in flight, 0 when none
       u32  ncalls              0 when none
       per call:
         u32  pledge            pledges table index, never a signal
         u8   has_outcome       0 for a call still queued or running
         when 1:
           u32  status          the call's GeasStatus
           u8   value_present   1 exactly when status is GEAS_OK, then the wire value
```

A signal's payload is its `ok` value, so it needs no field of its own. A blob with another magic or version, with counts that disagree with the registered descriptor, with a lapse flag on a latch that is not broken, with retry attempts beyond the declared budget, with a due time but no saved attempts, with a presence byte past 1, with a slot count that is not the course's, or with bytes left over is `GEAS_ERR_TYPE` and no instance is handed back, and a course frame on a contract with no course, or one whose course hash is not the registered one, is `GEAS_ERR_VERSION`. A retry due time further out than the pledge's own delay from now is held to that delay rather than refused. A remedy saved while it ran reads back failed with `GEAS_ERR_STATE`, see Parking a pass. The `state` column accepts 5, `Remedying`.

Every column is held to the registered descriptor's counts on a plain resume as much as through an amends block. A contract with no pledge has nothing to put in `latches` or `errs`, so a row that carries bytes in either is `GEAS_ERR_TYPE`, and a contract with no subcontract has nothing to put in `subtxn`, so bytes there are `GEAS_ERR_TYPE` as well, the way a vow column with bytes past the last vow is.

**Transactional fates.** The `subtxn` blob carries one byte per subcontract: 0 for an unopened episode, 2 for a committed episode, and 3 for a rolled back episode. The reader also accepts older rows whose 2 meant closed and uses the pledge latches to tell whether that episode committed. An open episode, byte 1, is refused at park and resume. An automatic break closes every open episode, so it can park with its errors and resume Broken.

**What crosses.** Eligibility stamps cross as they were, so a deadline keeps counting from where it started across the park: the clock is wall time, and an item whose `within` ran out while the row sat in the store lapses at the first tick after the resume. Lapse flags, Ok values, signal payloads, the latch order, and remedy outcomes cross unchanged, so a break after a resume runs the same remedies in the same order with the same arguments as it would have before the park.

**Parking live work.** Park refuses a running pledge body, a queued fulfillment, a completed but unwaited receipt, and an open transactional episode. It accepts a pledge retry wait and the fulfillment driving an active remedy pass, including park called from a remedy body or a host sleep callback. The snapshot leaves those live operations and their futures alone. Shutdown still joins workers, so the host wakes its own sleep callbacks when shutting down. A failed row write changes none of this progress. Arguments and frames stay in the parking process; the host supplies arguments again for every pending pledge after resume, and steps remain at least once.

**Parking a pass.** An instance that is Remedying parks. That happens when the park is called from inside a remedy body or from another thread while the pass waits out a retry delay. The remedies that finished are recorded with their outcomes and the ones not started are recorded as not started. The one in flight is recorded as running, and on resume it reads `GEAS_REMEDY_FAILED` with status `GEAS_ERR_STATE`: its body may or may not have finished, a remedy never runs twice, so the row answers on the safe side and the host reads it off the partial surface. The resumed instance reads `Remedying` and its first tick, explicit or through a call that ticks, runs the remedies still owed and latches Broken. A Broken record resumes with the pass closed and no remedy runs again.

**Resuming a format 1 row.** The row holds the latches and the signing second, and the workflow state is rebuilt from them. The signing second stands for the millisecond stamp. An item with no `after` is stamped eligible at that signing time, so its deadline counts from the one time the row kept. An item whose `after` has landed is stamped with the time of the resume, the first moment this runtime can vouch for. The fulfilled latches are ordered by declaration. No latch reads as a lapse and no Ok value exists, so a remedy that takes its pledge's Ok value has none to be handed after such a resume and is recorded failed with `GEAS_ERR_TYPE`, its body never run, while a remedy that takes no argument runs as usual.

## Amendment

A park row names the version and shape hash it was written under, and a contract at version M can resume a row written under an earlier version N through an amends table the module carries at the tail of its descriptor, one `GeasAmendDesc` per `amends N` block. A contract without a block emits nothing new, and its descriptor ends where it always did.

```c
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

/* The tail of GeasContractDesc, after course_hash */
uint32_t              namends;
const GeasAmendDesc*  amends;
```

`GEAS_CONTRACT_DESC_V4_SIZE` is the descriptor through `course_hash`, the course and no amends, and a descriptor whose reported size ends before `amends` carries no table, so every module built before the table existed loads as it did.

**The fields.** `from_version` and `from_shape_hash` are the version N and the shape hash a row has to carry for this block to take it. `from_course_hash` is the course hash version N carried, 0 when it had none, and `restart_course` is 1 when the block drops a parked frame and enters this version's course at step 0. The four old name lists are version N's tables in N's order, and the row is positional over them, so their lengths are N's counts: `old_vows` the vows, `old_items` the pledges then the signals in N's descriptor order, `old_subs` the subcontracts with a NULL entry for an anonymous one, and `old_remedies` the remedies. A list with a count of 0 may be NULL.

**The maps.** Each map holds one entry per old name, the index of this version's vow, pledges table entry, subcontract, or remedy it lands on, or -1 when it is dropped. A map entry is -1 or inside this version's table, no two entries name one index, and an item map entry that names a signal for an old pledge or the other way round is the compiler's to refuse, the runtime carries what it is told. A renamed member is a map entry to its new index, a retired one is -1, a member both versions share is a map entry to its own new index, and a member this version adds is the target of no entry.

**The seeds.** `latches` sets the starting state of items this version adds, each by its pledges table index: an index no item map entry names, at most once, with one `GEAS_AMEND_*` state. An added item without a seed starts pending. A fulfilled seed latches after every carried latch and carries no `Ok` value, so a remedy that takes one would record `GEAS_REMEDY_FAILED` with `GEAS_ERR_TYPE` without its body running; the compiler refuses that seed, and a handwritten table should too. A broken seed is a broken latch with no `Err` payload and no lapse mark.

**The vows function.** `vows_fn` is NULL when the block sets no vow. Otherwise it is called once, on the resuming thread with no lock held, `ctx` the fresh instance, `prior_vows` version N's vows decoded from the row in N's order with `nprior` their count, and `vows` this version's vow array with `nvows` its count, already holding every carried value and every default and zero in every other slot. It writes the slots its block names, with values built on `ctx` through the allocation helpers or constants, and may read any slot already filled; the runtime deep copies whatever changed onto the instance. A status other than `GEAS_OK` fails the resume with that status. After it returns a slot still zero is `GEAS_ERR_UNBOUND`, and a slot of another type than the vow declares is `GEAS_ERR_TYPE`. The compiled function checks `nprior` and `nvows` against the counts it was built for and answers `GEAS_ERR_TYPE` when they differ.

**The registrar.** `geas_register_contract` and its sized form validate the table before the contract is registered, and a table that fails is `GEAS_ERR_TYPE` with nothing registered: a nonzero `namends` with a NULL `amends`, a block whose version and hash are the contract's own, a NULL old name other than an anonymous subcontract, a map entry out of range or taken twice, a seed out of range, on an item a map entry already names, named twice, or with an unknown state, and two blocks for one version and hash, which would leave the resume guessing.

**The resume.** `geas_instance_resume` reads the row and compares its version and shape hash with the registered descriptor's. When they agree it is a plain resume. When they do not, the runtime looks for the block whose `from_version` and `from_shape_hash` are the row's, and when there is none the resume is `GEAS_ERR_VERSION` and the row stays as it was, so a module that does carry the block still resumes it later. A nonzero expected hash is checked against the registered hash, the version the instance resumes as. Through a block the runtime builds a fresh instance of this version and applies the block to it, all or nothing:

1. the row is decoded against the old counts, and a row whose counts disagree with the old name lists is `GEAS_ERR_TYPE`;
2. a row frame whose course hash is not `from_course_hash` is `GEAS_ERR_VERSION`, and so is a frame kept without `restart_course` for a course whose hash is not the registered one;
3. the vows are carried through `vow_map`, defaulted, and handed to `vows_fn`;
4. the sign time, the term lapse, and the latch counter cross, then each old item through `item_map`: its latch, its `Err` payload, its eligibility stamp, its lapse, its place in the latch order, its `Ok` value or signal payload, and its retry progress held to this version's retry budget;
5. each old subcontract's transactional fate and eligibility through `sub_map`, each old remedy's state, status, lapse, and `Err` payload through `remedy_map`;
6. the seeds, then eligibility under this version's terms;
7. the course frame kept at its step, dropped, or left for a fresh start at step 0.

Whatever maps to -1 is dropped. A failure at any step discards the instance, writes nothing, and answers its status, `GEAS_ERR_OOM` included, and the row is left for another try. A store backed contract reconciles its tables after the apply, since the `dsn` vow it opens is one the mapping fills.

**After the apply.** The runtime emits `GEAS_EVENT_AMENDED` with `item` NULL, `old_state` the row's version and `new_state` the registered one, then `GEAS_EVENT_RESUMED`, then evaluates this version's policy once over the mapped latches, and then starts the course, `COURSE_STARTED` for a fresh or restarted one and `COURSE_RESUMED` for a kept frame. A policy that breaks the instance there leaves it Remedying with no remedy run inside the resume, and its first tick runs the pass, the rule a row resumed Remedying already follows. The instance is an ordinary instance of the registered version from then on, and its next park writes a row of that version.

```c
uint32_t geas_contract_amended_from(GeasContract* c);
```

answers the version an instance was amended from, and 0 for an instance signed or resumed without a block, or a NULL handle. In Python it is `inst.amended_from()` and the event kind `GEAS_EVENT_AMENDED`; in dusk it is `contract_amended_from(c)`.

**The recorded shape.** `geas record FILE [--force]` writes `shapes/<Contract>.v<version>.shape` beside the source for every public contract the file declares, and a block is checked against that file. It is plain text, one record a line, every line ending in a newline:

```text
geas shape 3
contract NAME
version N
hash 0x<16 hex digits>          the shape hash
course 0x<16 hex digits>        the course hash, all zero when there is none
shape <canonical shape string>
vow NAME TYPE                   one per vow, declaration order
pledge NAME SIGNATURE OK        one per pledge, descriptor order
signal NAME PAYLOAD             one per signal, after every pledge
sub NAME                        one per subcontract, a bare "sub" when anonymous
remedy NAME TARGET              one per remedy, declaration order
type NAME DEFINITION            one per declared record or sum the boundary reaches, name order
```

Types are canonical type codes. A pledge's `OK` is the code of what a fulfilled pledge stores, the `T` of a `Result<T, E>` or the whole return otherwise, and a signal's `PAYLOAD` is its parameter codes in parentheses. A `type` record spells a record as `{field:code,...}` and a sum as `=Variant(field:code)|Variant`, the layout segments the shape line ends in. The first line names the format version, 3, and a reader refuses one it does not know: a file of format 1 or 2 records a shape hash this compiler does not compute, the first with no layouts and the second leaving out the subcontract members, the transactional flags, and the requirements, so the reader names the format and asks for `geas record` to be run again on the source of that version. The shape line has to hash to the recorded hash, so an edited file is refused rather than trusted. The vow, item, sub, and remedy lines are the old name lists of the `GeasAmendDesc` the compiler emits, in order. The head of the recorded shape of `skeleton/order_course.geas`:

```text
geas shape 3
contract OrderCourse
version 1
hash 0xa5fa4f6bf483ba55
course 0x9fb3ca94771a30a3
```

## Threading

The runtime owns a worker pool. `geas_runtime_init` takes an `GeasRuntimeConfig` whose `max_threads` sizes it, 0 or a NULL config selecting the default of 4 workers; a request beyond 256 is refused with `GEAS_ERR_TYPE`. The pool drains one unbounded queue, intrusive through the future itself so queuing allocates nothing. `geas_runtime_shutdown` stops intake, drains what is queued, joins every worker, and only then frees instances, futures, and modules; nothing else may be calling into the runtime by then.

The moments a fulfillment touches host memory are fixed whatever the pool does, and that is the whole point of the copy boundary: copy-in happens inside `geas_pledge_fulfill` on the caller's thread, write back happens inside the delivering `geas_future_wait` on the waiting thread, and the pool worker in between only ever sees instance owned memory.

**Serialization.** Every instance carries one recursive mutex. It covers fulfillment validation and copy-in, the whole thunk run, the outcome latch, every block list allocation, and break, so fulfillments against one instance serialize in queue order while distinct instances run truly in parallel. The allocation helpers, `geas_bytes` and everything built on it, take the instance lock themselves: recursive acquisition makes them safe inside a thunk, where the worker already holds the lock, and cold acquisition makes them safe from a host thread outside any fulfillment. A single `GeasValue` is still not a shared object; two threads mutating the same value is a host bug. `geas_vow_ref` is safe from any thread, with the standing caveat that the pointer it returns is instance owned and dangles after break.

**Wait semantics.** `geas_future_wait` blocks on the future's condition variable until the outcome exists, delivers exactly once, and performs the ref write back on the waiting thread after it lets the future's mutex go, the default write backs under the instance lock and every `write_back` callback after that lock is let go too, see By-reference arguments. A second wait is `GEAS_ERR_STATE`. The future struct itself is heap memory the runtime tracks per instance until release or shutdown, which is what makes a late wait on an unreleased handle safe; everything the delivered value points at is instance owned and dies at break, which is what keeps wait-before-break the rule for a host that wants the bytes.

**Break against in-flight work.** `geas_contract_break` takes the instance lock, so a thunk mid-run finishes and latches before the break proceeds; a task still queued finds the state already Broken when its worker arrives and never touches the freed heap. Before freeing anything the break forfeits every future not yet waited, delivered or not, to `GEAS_ERR_STATE` and clears its pointers into the instance heap. A fulfillment racing a break therefore resolves to exactly one of two outcomes: delivered before the break won, or `GEAS_ERR_STATE`; never a crash, never freed memory. An unwaited outcome is forfeited even if the pledge ran, so a host that wants a result waits before it breaks.

**Lock ordering.** The hierarchy runs instance above runtime, then future, taken downward and never upward: a caller instance's mutex, then any callee instance mutexes a cross-contract call nests below it, then the runtime lock over the descriptor, instance, and binding tables, then the per-future mutex. The pool's queue lock is a leaf taken with no other lock held. The runtime lock never sits above an instance mutex: `geas_contract_sign` resolves the descriptor and dispatch table under the runtime lock alone, builds the instance with no locks held, the allocation helpers touching only the fresh, unpublished instance's own mutex, and publishes it under the runtime lock again, which is what lets a thunk holding its instance lock sign through `geas_instance_runtime`. `geas_future_wait` waits on the future mutex alone; the lazy tick it runs first only tries the instance lock and skips the tick when it is busy, so a waiter never holds up an instance and never queues behind one.

The lock wrappers also maintain a thread local nesting count, one increment or decrement per hold, plus a zero check on release, so event delivery waits for the last instance lock on the thread. The same count decides who may wait out another thread's delivery: a thread holding exactly the one instance lock it took for the call, outside every hook, delivery, write back, and together body, waits on that instance's delivery condition, which gives the lock up whole for the wait, so the waiter holds nothing while the delivering thread takes the lock back to finish, and every place a reservation is let go broadcasts under that lock. A thread holding more than one instance lock is inside a body and keeps the refusal, so no wait ever sits above a lock another thread needs.

The workflow surface keeps this order and adds one habit. The clock and sleep functions are read under the runtime lock and called with it released. A remedy pass releases the instance lock between remedies for host snapshots. A retry delay, a pledge's or a remedy's, is waited with the instance lock released, so a sleeping worker holds up nothing, and the code that takes the lock back rechecks the instance before it touches anything. A remedy pass is owned by one thread from start to end; a `geas_contract_break` from another thread waits for it on a condition variable under the instance lock, holding nothing else.

Cancellation is published through an atomic flag before break takes the instance lock, and the body polls that flag without a lock. The mutex guarding the default retry wait is a leaf, released before taking any instance, runtime, or future lock.

Instance-to-instance nesting is safe for compiled code: a body only ever holds instances it signed itself, fresh and unshared, so the per-thread lock graph is a tree over recursive mutexes and a cycle cannot be constructed. The one deadlock v1 leaves possible is host made: two host bound bodies on different threads fulfilling against each other's shared instances in opposite orders. That needs instance pointers handed around outside the language, and v1 documents it instead of detecting it; `GEAS_ERR_DEADLOCK` stays reserved.

## Ownership

One rule, stated once: everything a fulfillment builds hangs off the contract instance it ran against, and `geas_contract_break` frees all of it in one walk. That covers thunk allocations, boxed payloads, concatenated strings, copied vow values, argument frames, and ref slots. A host that wants to keep a result copies it out before breaking; `geas_value_deep_copy` against another instance is one way. Arguments are deep copied at fulfillment entry, so the runtime holds nothing the host owns once the fulfill call returns, and the host may free or reuse its argument memory immediately. A frame the checked boundary refuses, and a signal payload it refuses, is taken back off the instance heap before the call returns, so a refused value leaves nothing behind. The instance struct itself stays valid for state queries until runtime shutdown; only its heap is gone. A future receipt is tracked per instance until release or shutdown, valid for its one wait even after break while unreleased, and dead after release, while the value inside it follows the instance heap.

Breaking latches the state at Broken, forfeits every unwaited future to `GEAS_ERR_STATE`, and every later fulfillment on the instance reports `GEAS_ERR_STATE` through its future or its synchronous return.

## Mangling

Every pledge a module compiles gets a mangled symbol:

```text
__geas_ash_{len}{contract}_{len}{pledge}_{sighash}_v{version}
```

`{contract}` and `{pledge}` are the declared names, which are identifiers and therefore valid symbol fragments, each one led by its length in bytes as a decimal number, `7Greeter` and `5greet`. The lengths are what keep the keyspace unambiguous: an identifier may hold an underscore, so without them pledge `b_c` of contract `A` and pledge `c` of contract `A_b` would spell one symbol. `{sighash}` is the 16 lowercase hex digits of the FNV-1a 64 hash, the shape hash's offset basis and prime, of the pledge's canonical signature spelling, `(codes)->code` in the type codes of The shape hash, followed by the layout segment of every declared record and sum that signature reaches, in name order, the same segments the shape string ends in. Parameter names never enter it. `{version}` is the contract's `version` attribute, 1 when the declaration carries none. The sig hash keeps two revisions of a pledge apart even when nothing about the surrounding contract moved, a field added to a record it takes included; the version keeps two shipped generations apart on purpose.

A remedy's mangled name is built the same way over the remedy's own signature, its pledge's Ok type as the parameter when it takes one, and the pledge it undoes does not enter it. A signal is never mangled, since nothing binds one: its pledges table entry carries a NULL `mangled`.

`Greeter.greet` takes a `String` and answers `Result<String, GreetError>`, and `GreetError` is a sum with the one variant `Empty`, so its signature hash is taken over `(S)->R(S,NGreetError)type GreetError=Empty;`, which is `5155c52e95ed821d`, and its symbol is `__geas_ash_7Greeter_5greet_5155c52e95ed821d_v1`. A signature that reaches no declared type, scalars and builtin composites of scalars only, hashes as its spelling alone: `()->V` hashes to `0e2ba22315b89a85`.

A schema row type is the one exception to the reach: a schema names a record the store owns, and a pledge that takes or answers a row spells it as an opaque name whose columns do not enter that pledge's signature hash. The schema's columns are a segment of the contract's shape string, `schema Accounts:id:I,balance:F,owner:S;`, so the layout of a schema row type is covered by the shape hash: a changed column moves the contract's hash, a host signing with the old hash gets `GEAS_ERR_VERSION`, and a row parked under the old schema no longer resumes as the current version.

The mangled name appears twice on purpose: as the thunk's C identifier inside the module and as the `mangled` string in the pledge descriptor, so a debugger and the runtime agree about what they are looking at. The thunk is `static`, the string is what the runtime keys, and so is every other symbol the emitted C defines save `geas_module_register`, the one the module exports: the `k{len}_{Contract}` stems of its descriptor tables and the `geas_clause{len}_{Contract}_{name}` functions of its clauses among them are private to the module, and their spelling is not part of the ABI.

The contract type itself has a mangled name too, in the same grammar with an empty symbol, spelled as its length `0`, and the shape hash where a pledge carries its signature hash:

```text
__geas_ash_{len}{contract}_0_{shapehash}_v{version}
```

The compiler does not emit this one; the runtime synthesizes it from the contract descriptor at registration, which it can do byte for byte because every piece is already in the descriptor: `Greeter` at version 1 is `__geas_ash_7Greeter_0_8b67a53d561ff526_v1`. It exists so the iname table has one keyspace: every discoverable thing, contract type or pledge, is one mangled string.

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

`geas_iname_dump` renders the whole table in its canonical text form, the discovery payload a foreign host reads. One entry per line in mangled name order, byte order as `strcmp` sees it, and this format is normative, a change to it is a wire change:

```text
{mangled} {kind} {shape_hash} v{version}\n
```

`{kind}` is spelled `contract` or `pledge`, `{shape_hash}` is 16 lowercase hex digits. The text is byte stable for a given set of registered contracts. `*need` receives the full size including the terminating NUL; when `cap` is at least that, the text and its NUL are written to `buf` and the call returns `GEAS_OK`, otherwise nothing is written and the call returns `GEAS_ERR_OOM`, so a NULL `buf` with `cap` 0 sizes the buffer. Loading the two skeleton modules dumps:

```text
__geas_ash_7Greeter_0_8b67a53d561ff526_v1 contract 8b67a53d561ff526 v1
__geas_ash_7Greeter_5greet_5155c52e95ed821d_v1 pledge 8b67a53d561ff526 v1
__geas_ash_7Greeter_5shout_5155c52e95ed821d_v1 pledge 8b67a53d561ff526 v1
__geas_ash_8Greeter2_0_ed0ccb47c5176d84_v2 contract ed0ccb47c5176d84 v2
__geas_ash_8Greeter2_5greet_0b2357619b406722_v2 pledge ed0ccb47c5176d84 v2
```

The length prefix sorts first, so every entry of `Greeter` lands before any of `Greeter2`, and a contract's own entry, its symbol spelled `0`, lands before its pledges.

## The freeze

`geas_runtime_freeze` latches the registration surface shut. After it returns, `geas_module_load`, `geas_register_contract`, `geas_pledge_bind`, and `geas_pledge_bind_user` report `GEAS_ERR_STATE`; `geas_contract_sign`, fulfillment, waits, vow reads, breaks, and every iname read continue unchanged. The call is idempotent and safe from any thread. The point is the discovery contract: once a host has frozen the runtime, the iname table it enumerates is the table every later sign resolves against, byte for byte, and no module loaded behind its back can move a name.

## The shape hash

A contract's shape hash is FNV-1a 64, offset basis `0xcbf29ce484222325` and prime `0x100000001b3`, over the contract's canonical shape string, the bytes of that string and nothing else. So the hash is exactly as stable as the string, and the string is built to be deterministic by construction: no table iteration order touches it, two builds of the same source agree byte for byte, and any change a host or a park row could feel moves it.

**Type codes.** Every type in the string is spelled in the checker's canonical code, the same spelling signature comparison uses:

| type | code | type | code |
|---|---|---|---|
| `Int` | `I` | `List<T>` | `L(T)` |
| `UInt` | `U` | `Option<T>` | `O(T)` |
| `Float` | `F` | `Map<K, V>` | `M(K,V)` |
| `Bool` | `B` | `Result<T, E>` | `R(T,E)` |
| `Byte` | `Y` | `(A, B, ...)` | `T(A,B,...)` |
| `Char` | `C` | a record, sum, or schema row named `Name` | `NName` |
| `String` | `S` | a contract named `Name` | `KName` |
| `Bytes` | `X` | a signature | `(A,B,...)->R` |
| `Unit` | `V` | | |
| `Duration` | `D` | | |
| `Time` | `W` | | |

A contract code never reaches the string, since a contract type is refused at every boundary, and neither does a pledge type, which this version of the language does not have.

**The segments.** The string is a run of segments, each ending in `;`, in this order:

1. `contract Name;`
2. `vow name:code;` for each vow, declaration order.
3. `pledge name:sig;` for each pledge in descriptor order, a subcontract's pledges in their place among the rest.
4. `schema Name:col:code,col:code;` for each schema, declaration order.
5. `signal name:sig;` for each signal, its signature answering `V`.
6. `remedy name:target:sig;` for each remedy, `target` the pledge it undoes.
7. `terms KIND NAME:term,term;` for each item that declares terms, the pledges first, then the subcontracts, the signals, and the remedies, each in descriptor order, `KIND` one of `pledge`, `sub`, `signal`, `remedy` and an anonymous subcontract named `#i` by its index. The terms are comma joined in source order: `within=MS`, `after=a+b`, and `retry=N/every=MS/backoff=F/cap=MS/on=V1+V2`, durations in milliseconds and the backoff in its shortest round trip spelling, so `2.0` is `2` and `2.50` is `2.5`.
8. `course;` when the contract declares a course.
9. `term MS;` when the contract carries a `term` attribute.
10. `sub Name:members;` for each subcontract, `sub Name:transactional:members;` for a transactional one, the members its pledges then its signals, comma joined.
11. `requirements fulfill=EXPR,partial=EXPR,break=EXPR;` when the source writes a requirements block, a line it leaves out spelled `-`, every binary node parenthesized and left associative, `&` and `|` and `!` over item and subcontract names.
12. One layout segment for every declared record and sum the boundary reaches, in name order, see Layouts below.

The version, every attribute but `term`, a vow's default, parameter names, clauses, bodies, the course's own statements, which the course hash covers, and amends blocks stay out, so a host's view of the contract is what the string pins.

**Layouts.** A type code names a declared record or sum, `NCard`, and nothing more, so after every other segment the string carries one layout segment per declared record and sum the boundary reaches, in name order. The boundary is every vow, every pledge, signal, and remedy signature, and a type reaches the types of its own fields and variant payloads and the type arguments of `List`, `Map`, `Option`, `Result`, and `Tuple`. A record is spelled with its fields in declaration order and a sum with its variants in declaration order, a variant with no payload as its bare name:

```text
type Name{field:code,field:code};
type Name=Variant(field:code,field:code)|Variant;
```

A type reached twice, a recursive one included, is spelled once. A contract whose boundary reaches no declared record or sum has no layout segment. Adding a field to a record a pledge takes therefore moves the contract's shape hash and that pledge's mangled name, and a park row written before the change belongs to another shape, which an amends block covers, see Amendment.

**A worked example.** `geas dump-shape`, the debug command the shape goldens run, prints the string, the hash, and every mangled name:

```text
$ geas dump-shape skeleton/hello.geas
// shape Greeter v1: contract Greeter;vow prefix:S;pledge greet:(S)->R(S,NGreetError);pledge shout:(S)->R(S,NGreetError);type GreetError=Empty;
#define GEAS_HASH_Greeter 0x8b67a53d561ff526ULL
#define GEAS_MANGLED_Greeter_greet "__geas_ash_7Greeter_5greet_5155c52e95ed821d_v1"
#define GEAS_MANGLED_Greeter_shout "__geas_ash_7Greeter_5shout_5155c52e95ed821d_v1"
```

So that is the small case, and every other segment shows up in a contract that uses the rest of the surface:

```geas
import std.store

record Card {
    number: String
    expiry: String
}

record Receipt {
    id: String
    amount: Int
    tags: List<String>
}

PayError is either Declined(reason: String) or Timeout or Gateway(code: Int)

contract Order [version: 3, term: 7d] {
    vow id: String
    vow dsn: String = "file:orders.db"
    vow limit: Option<Int>
    schema Lines { sku: String, qty: Int, price: Float }

    subcontract Payment {
        pledge authorize(card: Card) -> Result<String, PayError>
            within 30s
            retry 3 every 2s backoff 2.50 cap 30s on Timeout, Gateway
        pledge capture(token: String, amount: Int) -> Result<Receipt, PayError>
            after authorize
            within 1m
        signal settled(receipt: Receipt, at: Time) within 1h
    }

    subcontract Stock transactional {
        pledge hold(sku: String, qty: Int) -> Result<Unit, PayError> {
            return Ok(done())
        }
    }

    pledge ship(receipt: Receipt) -> Option<String>
        after Payment, Stock

    signal delivered(code: String) after ship within 3d

    remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>
        within 5m
        retry 2 every 1m

    clause done() -> Unit {
        return
    }

    requirements {
        fulfill: Payment && Stock && (ship || delivered)
        partial: Payment || Stock
        break: !Payment && !Stock
    }

    course {
        let token = authorize(Card { number: id, expiry: "12/30" })?
        let receipt = capture(token, 100)?
        hold("sku-1", 2)?
        let tracking = ship(receipt)
        let code = await delivered
    }
}
```

Its shape string, one line in the output and broken here at each segment for reading:

```text
contract Order;
vow id:S;vow dsn:S;vow limit:O(I);
pledge authorize:(NCard)->R(S,NPayError);
pledge capture:(S,I)->R(NReceipt,NPayError);
pledge hold:(S,I)->R(V,NPayError);
pledge ship:(NReceipt)->O(S);
schema Lines:sku:S,qty:I,price:F;
signal settled:(NReceipt,W)->V;
signal delivered:(S)->V;
remedy refund:capture:(NReceipt)->R(V,NPayError);
terms pledge authorize:within=30000,retry=3/every=2000/backoff=2.5/cap=30000/on=Timeout+Gateway;
terms pledge capture:after=authorize,within=60000;
terms pledge ship:after=Payment+Stock;
terms signal settled:within=3600000;
terms signal delivered:after=ship,within=259200000;
terms remedy refund:within=300000,retry=2/every=60000;
course;
term 604800000;
sub Payment:authorize,capture,settled;
sub Stock:transactional:hold;
requirements fulfill=((Payment&Stock)&(ship|delivered)),partial=(Payment|Stock),break=(!Payment&!Stock);
type Card{number:S,expiry:S};
type PayError=Declined(reason:S)|Timeout|Gateway(code:I);
type Receipt{id:S,amount:I,tags:L(S)};
```

hashes to `0x11f6cee48ef5b2a7`, and the names it mangles are

```text
__geas_ash_5Order_9authorize_654154b40815d02c_v3
__geas_ash_5Order_7capture_78637dc94df8dc3d_v3
__geas_ash_5Order_4hold_d5ef5838ba824176_v3
__geas_ash_5Order_4ship_9bbf824df9b5f205_v3
__geas_ash_5Order_6refund_52557414ab70af71_v3
```

`authorize` hashes `(NCard)->R(S,NPayError)type Card{number:S,expiry:S};type PayError=Declined(reason:S)|Timeout|Gateway(code:I);`, its spelling and the two layouts it reaches, and `refund` hashes `(NReceipt)->R(V,NPayError)` and the layouts of `PayError` and `Receipt`. The course hash of the same contract is `0x2582404b4ec1cabb`, see The course.

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
#ifndef GEAS_HEADER_HELLO_A430D84680AABD0B
#define GEAS_HEADER_HELLO_A430D84680AABD0B

/* contract Greeter, version 1
 * vow prefix: String (default)
 */
#define GEAS_HASH_Greeter 0x8b67a53d561ff526ULL

/* pledge greet(name: String) -> Result<String, GreetError> */
#define GEAS_MANGLED_Greeter_greet "__geas_ash_7Greeter_5greet_5155c52e95ed821d_v1"
```

One `GEAS_HASH_{Contract}` per public contract, the shape hash `geas_contract_sign` checks, and one `GEAS_MANGLED_{Contract}_{pledge}` per pledge, the exact string the iname table keys, each under a comment spelling the signature the way the source wrote it, an abstract pledge marked so the host knows to bind before signing. Internal contracts publish no descriptor and appear in no header. The hashes and mangled names come from the same canonical spellings the module emitter hashes, so a header and its module cannot disagree, and the text is byte stable for a given source, which is what lets `tests/golden/hello.geas.h` pin it.

A contract with remedies gets one more define per remedy after its pledges, `GEAS_MANGLED_{Contract}_{remedy}`, under a comment that names the pledge it undoes, `/* remedy refund for capture(receipt: Receipt) -> Result<Unit, PayError>, abstract */`, since a remedy binds through `geas_pledge_bind` exactly the way a pledge does. A contract without one gets the header it always got.

The include guard is `GEAS_HEADER_`, then the file's stem in capitals with every byte outside letters and digits spelled `_`, then `_` and the FNV-1a 64 hash of the stem itself in sixteen capital hex digits, so two stems that spell one identifier, `a-b` and `a_b`, still guard apart. A macro name is the contract and item names joined by underscores, which the mangled names keep apart with their lengths and a macro cannot, so a source in which two items would spell one macro is refused rather than written:

```text
error: geas: the header macro GEAS_MANGLED_A_b_c would name both pledge b_c of contract A and pledge c of contract A_b; rename one of them so each macro names one item
```


## Status codes

The numbers are part of the wire: `GEAS_OK` is 0 and the rest count up in the order of this table, `GEAS_ERR_STATE` 1 through `GEAS_ERR_STORE` 10.

| status | meaning |
|---|---|
| `GEAS_OK` | the operation ran; a pledge's Err is still `GEAS_OK` |
| `GEAS_ERR_STATE` | illegal in the contract's current state or from the calling thread, each reason listed below this table |
| `GEAS_ERR_TYPE` | argument count or type mismatch, at a thunk, at sign, at a ref, a value that does not conform to its declared type at the checked boundary, a body's answer that does not, a park row whose values do not, a type table the registrar refuses, an oversized pool request, a pledge call on a signal or a signal call on a pledge, or a workflow table the sign refuses |
| `GEAS_ERR_VERSION` | shape hash disagreed at sign, or a park row whose version and shape hash neither the registered contract nor one of its amends blocks accounts for |
| `GEAS_ERR_UNBOUND` | an abstract pledge, an abstract remedy, or an unsupplied vow at sign |
| `GEAS_ERR_NAME` | no contract, pledge, vow, or iname entry under that name |
| `GEAS_ERR_DEADLOCK` | reserved; v1 constructs no lock cycles and ships no detector |
| `GEAS_ERR_OOM` | allocation failed, a size past what memory can address, or a value past the node budget, `GEAS_VALUE_MAX_NODES` |
| `GEAS_ERR_LOAD` | a module dlopen refused, or one with no `geas_module_register` |
| `GEAS_ERR_NET` | reserved; the status numbers are pinned and this slot keeps its place |
| `GEAS_ERR_STORE` | the backend could not complete a store operation |

So `GEAS_ERR_STATE` is the one status for "not now, or not here", and a host tells its cases apart by where the call stood. Every reason it is answered, by the call that answers it:

- **Ordering.** A fulfillment or a signal whose `after` has not landed yet, the item not eligible. It becomes possible once what it waits on lands, so it is the one case a host may simply make again later.
- **A retry in flight.** A fulfillment of a pledge already waiting out its retry delay on another call, or made before the due time a parked retry recorded.
- **A lapse.** A fulfillment or a signal after its `within` ran out, the item latched broken by the deadline; `geas_partial_lapsed` reads 1 for it. Final.
- **The contract is over or not yet begun.** Any mutation on an Unsigned contract, on one that is Remedying, Broken, or reclaimed by an explicit break, or on one a release handed off; on a Remedying contract everything but a read. A fulfillment that ran into a break or a release under way, its receipt forfeited, and a wait on a receipt a break or a release forfeited. Final.
- **A course contract.** A host fulfillment of a pledge of an engine driven contract, whose course makes its calls; signals are still the host's. Final for that instance.
- **The wrong thread.** A fulfill, signal, park, break, or release from a hook on the instance it is delivering, or from a body, a write back callback, or a together body on an instance another thread is delivering; a course operation from anywhere but its course; `geas_course_wait` from a body, a hook, a delivery, or a thread holding an instance lock; `geas_runtime_stop_ticker` from the ticker or from a thread holding an instance lock; `geas_contract_free` from a body, a hook, or such a thread. The same call from a host thread is fine, and a host thread waits out another thread's delivery instead of being refused, see Events.
- **A cancel request.** Under `geas_contract_request_cancel`, an attempt whose outcome the pledge's `retry` would try again is not latched and its fulfillment answers `GEAS_ERR_STATE`, the pledge still pending; a default retry delay a request, a break, a release, or shutdown woke answers the same. The pledge can be fulfilled again; under a break or a release the instance is ending.
- **Park and autopark.** A park of an Unsigned or explicitly broken instance, or while a body runs, a receipt is unwaited, or a transactional episode is open. It becomes possible once that body returns, that receipt is waited, or that episode ends.
- **Release.** A release of an Unsigned instance, of one a break reclaimed or has pending, or of one already released or with a release pending; a break while a release is pending.
- **Receipts.** A second wait on one receipt, and `geas_future_release` of a receipt still pending or completed but unwaited.
- **Handles and the runtime.** `geas_contract_free` of an instance not yet ended or with anything still running on it, or of one a body signed; `geas_contract_request_cancel` after a break or a release reclaimed the instance; module load, registration, and bind after the freeze; a second ticker start or a stop with none running; a signal payload or a remedy status read before there is one; a claim whose row was rewritten under it eight times in a row.

## Stability

So from 1.0.0 on this document is a promise and not a snapshot. Every 1.x release of the compiler and the runtime keeps everything below exactly as it stands, and only a 2.0.0 may break any of it, the version number being the one place a break is allowed to show up.

**What is frozen.** The C surface of `geas.h` and `geas_abi.h`, and the two value codec calls of `geas_wire.h`, `geas_wire_encode_value` and `geas_wire_decode_value`: every function they declare, with its name, its parameters, and the meaning of every status it answers, and every struct, enum, macro, and typedef, with its layout, its field order and offsets, and its numbering. That covers the value representation, the thunk frame, every descriptor table and the size macros `GEAS_CONTRACT_DESC_V1_SIZE` through `GEAS_CONTRACT_DESC_V7_SIZE`, the type table, the event kinds, the status codes, the contract, item, remedy, and course states, the store calls and their terms, and the course entry points the emitter calls. The limits the headers name are frozen with their values, `GEAS_VALUE_MAX_DEPTH`, `GEAS_WIRE_MAX_DEPTH`, `GEAS_VALUE_MAX_NODES`, and `GEAS_SLOT_MIN_NODES_MAX`, so a value a 1.0.0 boundary accepts, a row a 1.0.0 park writes, and a course slot a 1.0.0 registrar takes are accepted, read back, and taken by every later 1.x runtime, and the status a value past one of them answers stays the status it answers now. Beside the headers, the formats a module, a host, or a database holds on to are frozen too: the mangling grammar, the shape string and its hash, the signature hash, the course hash, the text of `geas_iname_dump`, the value wire codec, the `geas_park` table and its row, the flow blob, the shape file `geas record` writes, and the names `geas emit-header` defines. So a module built by 1.0.0 loads into every later 1.x runtime, a host compiled against the 1.0.0 headers keeps working against every later 1.x runtime without a rebuild, a park row written by 1.0.0 resumes under every later 1.x runtime, and a contract whose source did not change keeps its shape hash, its course hash, and every mangled name byte for byte under every later 1.x compiler.

**How it still grows.** Now frozen does not mean finished, the surface grows, it just grows in three ways that leave every old reader right:

- A struct the runtime reads from a module grows only at its tail, the way the descriptor grew through seven tails, and each new tail gets its own `GEAS_CONTRACT_DESC_Vn_SIZE`. The registrar reads exactly the size a module reports and zero fills the rest, so an older module reads as one that declares nothing new, and a newer runtime never reads past what an older module wrote.
- An enum grows only at its end. A new event kind, status, or type tag takes the next number and every existing number keeps its meaning, so a host reads a value it does not know as one to pass over, and a host written that way keeps working when the enum grows.
- New behavior arrives as new calls. A call that exists keeps its signature and its contract, and a call that needs another parameter becomes a new call beside it.

The persistent formats follow the same rule. The flow blob takes a new version when it gains a field, and the reader keeps reading every version before it, the way it reads versions 2 through 5 today; the park table gains a column only through the same checked `ALTER TABLE` the `flow` and `gen` columns came in by; and a new shape string segment is added only for a declaration that did not exist before, so a contract that does not use it keeps its hash.

**What was never part of it.** Some of what the runtime and the compiler expose is there for the tests, the tools, or the build, and none of it is promised:

- `runtime/src/runtime_internal.h` and every `geas_rt_*` name. They are hidden from the shared object to begin with.
- Every symbol the emitted C defines other than `geas_module_register`, the one it exports: the `k{len}_{Contract}` table stems, the `geas_clause...` functions, the labels and slots of a course function, and the emitted C's text. The mangled names are frozen as the strings the descriptors carry and the iname table keys, not as C symbols, since every thunk is `static`.
- The text `geas_value_render` writes, which is for reading and for logs. Its size protocol and its NUL are frozen, its spelling is not.
- The output of `geas lex`, `geas parse`, and the debug commands the usage line leaves out, `geas dump-shape`, `geas dump-course`, `geas dump-types`, and the rest of the `dump-` family, and the wording of every diagnostic the compiler prints. The shape string `dump-shape` prints is frozen as a string; the lines around it are not.
- The store driver in `geas_store.h` and the frame calls in `geas_wire.h`, see Calls outside the ABI.
- The `sqlite3_*` symbols `libgeasrt` exports, which follow the vendored SQLite.
- The binding layers above the C surface, the Python package, the dusk host binding, and the generated protobuf, Go, and TypeScript. They sit on the frozen C surface, so they keep working, but their own spellings may still move in a minor release.

A host that sticks to the two headers and the formats above can treat a 1.x upgrade as a drop in, and that is the whole point of drawing the line here.
