/* conform.c: the checked boundary. A contract whose descriptor carries a type
 * table holds every value that crosses into its code or into the runtime's
 * keeping to the declared type all the way down: a fulfillment's frame, a
 * body's return, a remedy's argument and return, a signal's payload, a vow,
 * and every value a park row brings back. The walk below is the one
 * judgement all of those share; the boundary hooks at the end name the type
 * each of them is held to.
 *
 * Two modes ride one walk. A table index holds the value to that type: the
 * tag it carries, the shape of its arm, and every value inside it, each
 * against its own index. GEAS_NO_TYPE holds the value to the structural facts
 * every value keeps whatever its type, a slot the table has no spelling for:
 * a Bool is 0 or 1, a Char a Unicode scalar value, a String valid UTF-8, an
 * arm with a length has data behind it, a list's elements carry its elem_ty,
 * a map's keys are distinct, a tag rides only on a sum shaped value, None
 * carries no box. A pledge reference and an instance handle never conform in
 * either mode; neither is a value a boundary carries. Everything that
 * conforms is a value the wire codec encodes, so it can always be parked.
 *
 * The walk recurses and is bounded at GEAS_VALUE_MAX_DEPTH nested values,
 * so a recursive type or a hostile value cannot run the stack away. The work
 * is linear in the value's size, a map's duplicate key check n log n in its
 * pair count. Nothing here takes a lock or allocates on an instance: the
 * caller holds whatever lock keeps the value still, and the one allocation,
 * the scratch of a large map's key sort, is plain heap freed before return.
 * The registrar's check of the table itself lives here too, so the walk can
 * trust every index it follows. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

/* A map with this many pairs or fewer is checked for duplicate keys by
 * comparing every pair of keys, which beats a sort at that size */
#define MAP_SCAN_PAIRS 16

/* The largest Unicode scalar value */
#define UNICODE_MAX 0x10FFFFu

/* ---- UTF-8 and scalars ---- */

/* Decodes Unicode scalars, rejecting overlong encodings, surrogates, values
 * above U+10FFFF, and a sequence the bytes cut short. An embedded zero is
 * ordinary ASCII */
int geas_rt_utf8_valid(const uint8_t* ptr, uint64_t len) {
    uint64_t offset = 0;
    while (offset < len) {
        uint8_t first = ptr[offset++];
        if (first < 0x80) continue;
        uint32_t scalar;
        uint32_t minimum;
        unsigned following;
        if (first >= 0xc2 && first <= 0xdf) {
            scalar = first & 0x1f; minimum = 0x80; following = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            scalar = first & 0x0f; minimum = 0x800; following = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            scalar = first & 0x07; minimum = 0x10000; following = 3;
        } else {
            return 0;
        }
        if (len - offset < following) return 0;
        for (unsigned i = 0; i < following; i++) {
            uint8_t next = ptr[offset++];
            if ((next & 0xc0) != 0x80) return 0;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum || scalar > UNICODE_MAX ||
            (scalar >= 0xd800 && scalar <= 0xdfff)) return 0;
    }
    return 1;
}

static int scalar_valid(uint32_t ch) {
    return ch <= UNICODE_MAX && (ch < 0xD800 || ch > 0xDFFF);
}

/* A tag a value that crosses may carry: an instance handle and a pledge
 * reference never cross, and anything past the enum is no tag at all. The
 * wire codec holds the same set */
static int tag_crosses(uint32_t ty) {
    return (ty <= GEAS_TY_SUM && ty != GEAS_TY_PLEDGE_REF) || ty == GEAS_TY_BYTES;
}

/* Only the sum shaped types, Option, Result, and a declared sum, carry a
 * tag; every other value carries 0 there */
static int tag_is_variant(uint32_t ty) {
    return ty == GEAS_TY_OPTION || ty == GEAS_TY_RESULT || ty == GEAS_TY_SUM;
}

static int tag_keyable(uint32_t ty) {
    switch (ty) {
    case GEAS_TY_INT:
    case GEAS_TY_UINT:
    case GEAS_TY_BOOL:
    case GEAS_TY_BYTE:
    case GEAS_TY_CHAR:
    case GEAS_TY_STRING:
        return 1;
    default:
        return 0;
    }
}

/* ---- map keys ---- */

/* Orders two keys of one keyable tag the way geas_value_eq tells them apart:
 * by value for the scalars, by bytes then length for a String */
static int key_cmp(const void* pa, const void* pb) {
    const GeasValue* a = *(const GeasValue* const*)pa;
    const GeasValue* b = *(const GeasValue* const*)pb;
    if (a->ty != b->ty) return a->ty < b->ty ? -1 : 1;
    switch ((GeasTypeTag)a->ty) {
    case GEAS_TY_INT:
        return a->as.i < b->as.i ? -1 : a->as.i > b->as.i;
    case GEAS_TY_BOOL:
    case GEAS_TY_BYTE:
        return a->as.b < b->as.b ? -1 : a->as.b > b->as.b;
    case GEAS_TY_CHAR:
        return a->as.ch < b->as.ch ? -1 : a->as.ch > b->as.ch;
    case GEAS_TY_STRING: {
        uint64_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int by_bytes = n ? memcmp(a->as.s.ptr, b->as.s.ptr, n) : 0;
        if (by_bytes != 0) return by_bytes;
        return a->as.s.len < b->as.s.len ? -1 : a->as.s.len > b->as.s.len;
    }
    default:
        return a->as.u < b->as.u ? -1 : a->as.u > b->as.u;
    }
}

/* Small maps compare every pair of keys, O(n^2) with n at most
 * MAP_SCAN_PAIRS; larger ones sort pointers to the keys and compare
 * neighbours, O(n log n) time and n pointers of scratch */
GeasStatus geas_rt_map_keys_distinct(const GeasValue* data, uint64_t npairs) {
    if (npairs < 2) return GEAS_OK;
    if (npairs <= MAP_SCAN_PAIRS) {
        for (uint64_t i = 1; i < npairs; i++) {
            for (uint64_t j = 0; j < i; j++) {
                if (geas_value_eq(&data[2 * i], &data[2 * j])) return GEAS_ERR_TYPE;
            }
        }
        return GEAS_OK;
    }
    if (npairs > SIZE_MAX / sizeof(const GeasValue*)) return GEAS_ERR_OOM;
    const GeasValue** keys = malloc((size_t)npairs * sizeof(*keys));
    if (!keys) return GEAS_ERR_OOM;
    for (uint64_t i = 0; i < npairs; i++) keys[i] = &data[2 * i];
    qsort(keys, (size_t)npairs, sizeof(*keys), key_cmp);
    GeasStatus st = GEAS_OK;
    for (uint64_t i = 1; i < npairs && st == GEAS_OK; i++) {
        if (key_cmp(&keys[i - 1], &keys[i]) == 0) st = GEAS_ERR_TYPE;
    }
    free(keys);
    return st;
}

/* ---- the walk ---- */

static GeasStatus conform_rec(const GeasContractDesc* d, uint32_t ty, const GeasValue* v,
                              uint32_t depth);

/* The node behind an index, NULL for GEAS_NO_TYPE. The registrar checked
 * every index the table holds, so a miss here is a caller's index */
static const GeasTypeNode* node_of(const GeasContractDesc* d, uint32_t ty) {
    if (ty == GEAS_NO_TYPE || ty >= d->ntypes) return NULL;
    return &d->types[ty];
}

/* An arm with a length has data behind it */
static int arm_present(const GeasValue* v) {
    return v->as.list.len == 0 || v->as.list.data != NULL;
}

static GeasStatus conform_text(const GeasValue* v, int is_utf8) {
    if (v->as.s.len && !v->as.s.ptr) return GEAS_ERR_TYPE;
    if (is_utf8 && !geas_rt_utf8_valid(v->as.s.ptr, v->as.s.len)) return GEAS_ERR_TYPE;
    return GEAS_OK;
}

/* The slots of a tuple, a record, or a sum's variant, each against its own
 * index, or each held to the structural facts when kids is NULL */
static GeasStatus conform_slots(const GeasContractDesc* d, const uint32_t* kids,
                                const GeasValue* v, uint32_t depth) {
    if (!arm_present(v)) return GEAS_ERR_TYPE;
    const GeasValue* e = (const GeasValue*)v->as.list.data;
    for (uint64_t i = 0; i < v->as.list.len; i++) {
        GeasStatus st = conform_rec(d, kids ? kids[i] : GEAS_NO_TYPE, &e[i], depth + 1);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

static GeasStatus conform_list(const GeasContractDesc* d, const GeasTypeNode* n,
                               const GeasValue* v, uint32_t depth) {
    if (!arm_present(v) || v->as.list.len > v->as.list.cap) return GEAS_ERR_TYPE;
    if (!tag_crosses(v->as.list.elem_ty)) return GEAS_ERR_TYPE;
    uint32_t elem = n ? n->kids[0] : GEAS_NO_TYPE;
    if (n && v->as.list.elem_ty != d->types[elem].tag) return GEAS_ERR_TYPE;
    const GeasValue* e = (const GeasValue*)v->as.list.data;
    for (uint64_t i = 0; i < v->as.list.len; i++) {
        if (e[i].ty != v->as.list.elem_ty) return GEAS_ERR_TYPE;
        GeasStatus st = conform_rec(d, elem, &e[i], depth + 1);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

static GeasStatus conform_map(const GeasContractDesc* d, const GeasTypeNode* n,
                              const GeasValue* v, uint32_t depth) {
    if (!arm_present(v) || v->as.list.len % 2) return GEAS_ERR_TYPE;
    if (!tag_keyable(v->as.list.elem_ty)) return GEAS_ERR_TYPE;
    uint32_t key = n ? n->kids[0] : GEAS_NO_TYPE;
    uint32_t val = n ? n->kids[1] : GEAS_NO_TYPE;
    if (n && v->as.list.elem_ty != d->types[key].tag) return GEAS_ERR_TYPE;
    const GeasValue* e = (const GeasValue*)v->as.list.data;
    for (uint64_t i = 0; i < v->as.list.len; i += 2) {
        if (e[i].ty != v->as.list.elem_ty) return GEAS_ERR_TYPE;
        GeasStatus st = conform_rec(d, key, &e[i], depth + 1);
        if (st == GEAS_OK) st = conform_rec(d, val, &e[i + 1], depth + 1);
        if (st != GEAS_OK) return st;
    }
    return geas_rt_map_keys_distinct(e, v->as.list.len / 2);
}

static GeasStatus conform_fields(const GeasContractDesc* d, const GeasTypeNode* n,
                                 const GeasValue* v, uint32_t depth) {
    if (n && v->as.list.len != n->nkids) return GEAS_ERR_TYPE;
    return conform_slots(d, n ? n->kids : NULL, v, depth);
}

static GeasStatus conform_sum(const GeasContractDesc* d, const GeasTypeNode* n,
                              const GeasValue* v, uint32_t depth) {
    if (!n) return conform_slots(d, NULL, v, depth);
    if (v->tag >= n->nvariants) return GEAS_ERR_TYPE;
    const GeasTypeVariant* var = &n->variants[v->tag];
    if (v->as.list.len != var->nfields) return GEAS_ERR_TYPE;
    return conform_slots(d, var->fields, v, depth);
}

/* None carries a NULL box and nothing else, the spelling the wire codec
 * parks. The box of a None is never followed, so one that is not NULL is
 * refused without being read. A value the runtime copies before it checks,
 * a fulfillment's argument, a signal's payload, a vow binding, has already
 * had such a box dropped by the copy; a body's answer is not copied and is
 * held to the rule as written */
static GeasStatus conform_option(const GeasContractDesc* d, const GeasTypeNode* n,
                                 const GeasValue* v, uint32_t depth) {
    if (v->tag > 1) return GEAS_ERR_TYPE;
    if (v->tag == 0) return v->as.box ? GEAS_ERR_TYPE : GEAS_OK;
    if (!v->as.box) return GEAS_ERR_TYPE;
    return conform_rec(d, n ? n->kids[0] : GEAS_NO_TYPE, v->as.box, depth + 1);
}

/* A Result arm of Unit may ride with no box, the way a body answers Ok or
 * Err of Unit; every other arm carries its payload in the box */
static GeasStatus conform_result(const GeasContractDesc* d, const GeasTypeNode* n,
                                 const GeasValue* v, uint32_t depth) {
    if (v->tag > 1) return GEAS_ERR_TYPE;
    uint32_t inner = n ? n->kids[v->tag] : GEAS_NO_TYPE;
    if (!v->as.box) {
        if (!n || d->types[inner].tag == GEAS_TY_UNIT) return GEAS_OK;
        return GEAS_ERR_TYPE;
    }
    return conform_rec(d, inner, v->as.box, depth + 1);
}

static GeasStatus conform_rec(const GeasContractDesc* d, uint32_t ty, const GeasValue* v,
                              uint32_t depth) {
    /* The outermost value is depth 0, so this admits exactly
     * GEAS_VALUE_MAX_DEPTH levels, the count the wire codec admits too */
    if (depth >= GEAS_VALUE_MAX_DEPTH) return GEAS_ERR_TYPE;
    const GeasTypeNode* n = node_of(d, ty);
    if (ty != GEAS_NO_TYPE && !n) return GEAS_ERR_TYPE;
    if (n && v->ty != n->tag) return GEAS_ERR_TYPE;
    if (v->tag != 0 && !tag_is_variant(v->ty)) return GEAS_ERR_TYPE;
    switch ((GeasTypeTag)v->ty) {
    case GEAS_TY_UNIT:
    case GEAS_TY_INT:
    case GEAS_TY_UINT:
    case GEAS_TY_FLOAT:
    case GEAS_TY_BYTE:
        return GEAS_OK;
    case GEAS_TY_BOOL:
        return v->as.b <= 1 ? GEAS_OK : GEAS_ERR_TYPE;
    case GEAS_TY_CHAR:
        return scalar_valid(v->as.ch) ? GEAS_OK : GEAS_ERR_TYPE;
    case GEAS_TY_STRING:
        return conform_text(v, 1);
    case GEAS_TY_BYTES:
        return conform_text(v, 0);
    case GEAS_TY_LIST:
        return conform_list(d, n, v, depth);
    case GEAS_TY_MAP:
        return conform_map(d, n, v, depth);
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
        return conform_fields(d, n, v, depth);
    case GEAS_TY_SUM:
        return conform_sum(d, n, v, depth);
    case GEAS_TY_OPTION:
        return conform_option(d, n, v, depth);
    case GEAS_TY_RESULT:
        return conform_result(d, n, v, depth);
    default:
        /* A pledge reference, an instance handle, or no tag at all */
        return GEAS_ERR_TYPE;
    }
}

GeasStatus geas_rt_conform(const GeasContractDesc* d, uint32_t ty, const GeasValue* v) {
    if (!d || !v) return GEAS_ERR_TYPE;
    return conform_rec(d, ty, v, 0);
}

/* ---- the registrar's check of the table ---- */

static int index_ok(const GeasContractDesc* d, uint32_t i) {
    return i < d->ntypes;
}

static int slot_ok(const GeasContractDesc* d, uint32_t i) {
    return i == GEAS_NO_TYPE || index_ok(d, i);
}

static int indices_ok(const GeasContractDesc* d, const uint32_t* xs, uint32_t n) {
    if (n > 0 && !xs) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!index_ok(d, xs[i])) return 0;
    }
    return 1;
}

/* The kid count a tag carries, -1 for any count */
static int64_t kids_for(uint32_t tag) {
    switch (tag) {
    case GEAS_TY_LIST:
    case GEAS_TY_OPTION:
        return 1;
    case GEAS_TY_MAP:
    case GEAS_TY_RESULT:
        return 2;
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
        return -1;
    default:
        return 0;
    }
}

/* A tag a table node may carry. An instance and a pledge reference never
 * cross a boundary, so no table spells either; a node that does is refused
 * at registration rather than at every value that meets it */
static int tag_known(uint32_t tag) {
    return tag_crosses(tag);
}

static int sum_ok(const GeasContractDesc* d, const GeasTypeNode* n) {
    if (n->nkids != 0 || n->nvariants == 0 || !n->variants) return 0;
    for (uint32_t v = 0; v < n->nvariants; v++) {
        if (!indices_ok(d, n->variants[v].fields, n->variants[v].nfields)) return 0;
    }
    return 1;
}

static int node_ok(const GeasContractDesc* d, const GeasTypeNode* n) {
    if (!tag_known(n->tag)) return 0;
    if (n->tag == GEAS_TY_SUM) return sum_ok(d, n);
    if (n->nvariants != 0) return 0;
    int64_t want = kids_for(n->tag);
    if (want >= 0 && n->nkids != (uint32_t)want) return 0;
    if (!indices_ok(d, n->kids, n->nkids)) return 0;
    if (n->tag == GEAS_TY_MAP && !tag_keyable(d->types[n->kids[0]].tag)) return 0;
    return 1;
}

static int item_ok(const GeasContractDesc* d, const GeasItemTypes* it, uint32_t nparams) {
    if (it->nparams != nparams) return 0;
    if (nparams > 0 && !it->params) return 0;
    for (uint32_t i = 0; i < nparams; i++) {
        if (!slot_ok(d, it->params[i])) return 0;
    }
    return slot_ok(d, it->ret);
}

static int items_ok(const GeasContractDesc* d) {
    if (d->npledges > 0 && !d->item_types) return 0;
    for (uint32_t i = 0; i < d->npledges; i++) {
        const GeasItemTypes* it = &d->item_types[i];
        if (!item_ok(d, it, d->pledges[i].nargs)) return 0;
        if (geas_rt_is_signal(d, i) && it->ret != GEAS_NO_TYPE) return 0;
    }
    return 1;
}

static int vows_ok(const GeasContractDesc* d) {
    if (d->nvows > 0 && !d->vow_types) return 0;
    for (uint32_t j = 0; j < d->nvows; j++) {
        uint32_t t = d->vow_types[j];
        if (!slot_ok(d, t)) return 0;
        if (t != GEAS_NO_TYPE && d->types[t].tag != d->vows[j].ty) return 0;
    }
    return 1;
}

static int remedies_ok(const GeasContractDesc* d) {
    if (d->nremedies > 0 && !d->remedy_types) return 0;
    for (uint32_t r = 0; r < d->nremedies; r++) {
        uint32_t nparams = d->remedies[r].has_param ? 1 : 0;
        if (!item_ok(d, &d->remedy_types[r], nparams)) return 0;
    }
    return 1;
}

/* Every node's tag known and its kids in range and of the count its tag
 * carries, a map keyed by a keyable scalar, a sum with a variant table whose
 * fields are in range, then every item, vow, and remedy slot an index or
 * GEAS_NO_TYPE, the counts matching the descriptor's own, and no return on a
 * signal. A kid that points back at its own node or an ancestor is a
 * recursive type and legal. ntypes 0 is a descriptor with no table and
 * passes untouched */
GeasStatus geas_rt_validate_types(const GeasContractDesc* d) {
    if (d->ntypes == 0) return GEAS_OK;
    if (!d->types || d->ntypes == GEAS_NO_TYPE) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < d->ntypes; i++) {
        if (!node_ok(d, &d->types[i])) return GEAS_ERR_TYPE;
    }
    if (!items_ok(d) || !vows_ok(d) || !remedies_ok(d)) return GEAS_ERR_TYPE;
    return GEAS_OK;
}

/* ---- the boundaries ---- */

int geas_rt_has_types(const GeasContractDesc* d) {
    return d && d->ntypes > 0;
}

/* The Ok and Err types of a return index. A Result splits into its arms; any
 * other type is the Ok type itself, and then no Err payload conforms, which
 * err_ty reports as GEAS_NO_TYPE with *is_err_legal 0 */
static void outcome_types(const GeasContractDesc* d, uint32_t ret, uint32_t* ok_ty,
                          uint32_t* err_ty, int* is_err_legal) {
    *ok_ty = ret;
    *err_ty = GEAS_NO_TYPE;
    *is_err_legal = 1;
    if (ret == GEAS_NO_TYPE) return;
    const GeasTypeNode* n = &d->types[ret];
    if (n->tag == GEAS_TY_RESULT) {
        *ok_ty = n->kids[0];
        *err_ty = n->kids[1];
        return;
    }
    *is_err_legal = 0;
}

GeasStatus geas_rt_check_args(const GeasContract* c, uint32_t pidx, const GeasValue* frame,
                              size_t nargs) {
    const GeasContractDesc* d = c->desc;
    if (!geas_rt_has_types(d)) return GEAS_OK;
    const GeasItemTypes* it = &d->item_types[pidx];
    if (nargs != it->nparams || (nargs > 0 && !frame)) return GEAS_ERR_TYPE;
    for (size_t i = 0; i < nargs; i++) {
        GeasStatus st = geas_rt_conform(d, it->params[i], &frame[i]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

GeasStatus geas_rt_check_return(const GeasContract* c, uint32_t pidx, const GeasValue* out) {
    const GeasContractDesc* d = c->desc;
    if (!geas_rt_has_types(d)) return GEAS_OK;
    return geas_rt_conform(d, d->item_types[pidx].ret, out);
}

/* A signal's payload is Unit for no parameter, the value for one, and a
 * Tuple of the arguments for several */
GeasStatus geas_rt_check_payload(const GeasContract* c, uint32_t pidx,
                                 const GeasValue* payload) {
    const GeasContractDesc* d = c->desc;
    if (!geas_rt_has_types(d)) return GEAS_OK;
    const GeasItemTypes* it = &d->item_types[pidx];
    if (it->nparams == 0) return payload->ty == GEAS_TY_UNIT ? GEAS_OK : GEAS_ERR_TYPE;
    if (it->nparams == 1) return geas_rt_conform(d, it->params[0], payload);
    if (payload->ty != GEAS_TY_TUPLE || payload->as.list.len != it->nparams) {
        return GEAS_ERR_TYPE;
    }
    if (!payload->as.list.data) return GEAS_ERR_TYPE;
    const GeasValue* slots = (const GeasValue*)payload->as.list.data;
    for (uint32_t i = 0; i < it->nparams; i++) {
        GeasStatus st = geas_rt_conform(d, it->params[i], &slots[i]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

GeasStatus geas_rt_check_vow(const GeasContractDesc* d, uint32_t j, const GeasValue* v) {
    if (!geas_rt_has_types(d)) return GEAS_OK;
    return geas_rt_conform(d, d->vow_types[j], v);
}

GeasStatus geas_rt_check_remedy_arg(const GeasContract* c, uint32_t r, const GeasValue* arg) {
    const GeasContractDesc* d = c->desc;
    if (!geas_rt_has_types(d)) return GEAS_OK;
    const GeasItemTypes* it = &d->remedy_types[r];
    if (it->nparams == 0) return GEAS_OK;
    return geas_rt_conform(d, it->params[0], arg);
}

GeasStatus geas_rt_check_remedy_return(const GeasContract* c, uint32_t r,
                                       const GeasValue* out) {
    const GeasContractDesc* d = c->desc;
    if (!geas_rt_has_types(d)) return GEAS_OK;
    return geas_rt_conform(d, d->remedy_types[r].ret, out);
}

/* ---- what a park row brought back ---- */

/* All zero is the absent spelling of an Err payload, a pledge that never
 * broke with one and a lapse alike, the same rule the park writer reads */
static int value_absent(const GeasValue* v) {
    static const GeasValue zero;
    return memcmp(v, &zero, sizeof(zero)) == 0;
}

static GeasStatus check_err(const GeasContractDesc* d, uint32_t ret, const GeasValue* err) {
    if (value_absent(err)) return GEAS_OK;
    uint32_t ok_ty, err_ty;
    int is_err_legal;
    outcome_types(d, ret, &ok_ty, &err_ty, &is_err_legal);
    if (!is_err_legal) return GEAS_ERR_TYPE;
    return geas_rt_conform(d, err_ty, err);
}

static GeasStatus check_ok(const GeasContractDesc* d, uint32_t ret, const GeasValue* ok) {
    uint32_t ok_ty, err_ty;
    int is_err_legal;
    outcome_types(d, ret, &ok_ty, &err_ty, &is_err_legal);
    return geas_rt_conform(d, ok_ty, ok);
}

/* One item's stored values: a signal's payload, a pledge's Ok value and Err
 * payload */
static GeasStatus check_item(const GeasContract* c, uint32_t i) {
    const GeasContractDesc* d = c->desc;
    const GeasFlow* f = &c->flow;
    uint32_t ret = d->item_types[i].ret;
    int is_ok_set = f->pledge_ok_set && f->pledge_ok_set[i];
    if (geas_rt_is_signal(d, i)) {
        return is_ok_set ? geas_rt_check_payload(c, i, &f->pledge_ok[i]) : GEAS_OK;
    }
    if (c->pledge_err) {
        GeasStatus st = check_err(d, ret, &c->pledge_err[i]);
        if (st != GEAS_OK) return st;
    }
    return is_ok_set ? check_ok(d, ret, &f->pledge_ok[i]) : GEAS_OK;
}

/* A course's slots carry no declared type, so they are held to the
 * structural facts; a together's outcomes are held to their pledge's return */
static GeasStatus check_course(const GeasContract* c) {
    const GeasCourse* k = c->course;
    if (!k) return GEAS_OK;
    for (uint32_t s = 0; k->frame.slots && s < k->frame.nslots; s++) {
        GeasStatus st = geas_rt_conform(c->desc, GEAS_NO_TYPE, &k->frame.slots[s]);
        if (st != GEAS_OK) return st;
    }
    for (uint32_t i = 0; k->join && i < k->join_n; i++) {
        const GeasJoinCall* call = &k->join[i];
        if (call->state != JOIN_DONE || call->status != GEAS_OK) continue;
        GeasStatus st = geas_rt_check_return(c, call->pledge, &call->value);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* Every value a resume, a claim, or an amend decoded onto a fresh instance:
 * the vows, each item's stored values, each failed remedy's Err payload, and
 * the course frame. The instance is unpublished, so no lock is needed */
GeasStatus geas_rt_check_instance(const GeasContract* c) {
    const GeasContractDesc* d = c->desc;
    if (!geas_rt_has_types(d)) return GEAS_OK;
    for (uint32_t j = 0; c->vow_vals && j < d->nvows; j++) {
        GeasStatus st = geas_rt_check_vow(d, j, &c->vow_vals[j]);
        if (st != GEAS_OK) return st;
    }
    for (uint32_t i = 0; i < d->npledges; i++) {
        GeasStatus st = check_item(c, i);
        if (st != GEAS_OK) return st;
    }
    for (uint32_t r = 0; c->flow.remedy_err && r < d->nremedies; r++) {
        GeasStatus st = check_err(d, d->remedy_types[r].ret, &c->flow.remedy_err[r]);
        if (st != GEAS_OK) return st;
    }
    return check_course(c);
}
