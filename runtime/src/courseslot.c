/* courseslot.c: the course slot types.
 *
 * A compiled course reads every slot of its frame as the type the compiler
 * gave it, with no check of its own, so a frame is only sound when every
 * slot holds a value of that type. The descriptor's course_slot_types names
 * one type table index per slot, and two rules keep the frame sound:
 *
 *   1. Every slot starts at the smallest value of its type, never at a bare
 *      Unit, so a slot no instruction wrote yet already conforms. An Int is
 *      0, a String empty, a List or a Map empty, an Option None, a record
 *      its fields at their smallest, a sum its variant whose payload nests
 *      least, and a Result its arm that nests least.
 *   2. Every slot a resume, a claim, or an amend brings back from a park row
 *      is held to its type, see check_course in conform.c.
 *
 * So a row can never hand the course a value it would read as another type,
 * whatever step the row names, and a slot the course had not written yet when
 * the row was taken comes back as the smallest value it started as.
 *
 * The smallest value is found through the height of each node, the fewest
 * levels any finite value of the type nests: 1 for a scalar, an empty List
 * or Map, and a None, one more than the deepest kid for a Tuple and a record,
 * one more than the least of the arms for a Result, and one more than the
 * variant whose fields nest least for a sum. The heights settle by
 * relaxation from "no finite value" downward, at most one pass per node. A
 * type with no finite value, a record that holds itself with no List or
 * Option between, keeps no height, and the registrar refuses a slot of such a
 * type, as it refuses one whose smallest value nests past
 * GEAS_VALUE_MAX_DEPTH.
 *
 * Nesting alone does not bound the size: a record that holds the next record
 * twice doubles at every level. So the registrar also counts the nodes of
 * every smallest value, one for the node itself plus the smallest value of
 * every kid the value carries, the kids the height rule chose for a sum and a
 * Result. Each chosen kid sits lower than its parent, so the counts settle by
 * the same relaxation, and they saturate just past GEAS_SLOT_MIN_NODES_MAX,
 * the most a slot's smallest value may hold. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

/* The height of a type with no finite value */
#define HEIGHT_NONE UINT32_MAX

/* The deepest of a run of kids, HEIGHT_NONE when any has no finite value */
static uint32_t deepest(const uint32_t* heights, const uint32_t* kids, uint32_t n) {
    uint32_t most = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h = heights[kids[i]];
        if (h == HEIGHT_NONE) return HEIGHT_NONE;
        if (h > most) most = h;
    }
    return most;
}

/* The variant of a sum whose fields nest least, the first of equals */
static uint32_t least_variant(const uint32_t* heights, const GeasTypeNode* n, uint32_t* height) {
    uint32_t best = 0;
    *height = HEIGHT_NONE;
    for (uint32_t v = 0; v < n->nvariants; v++) {
        uint32_t h = deepest(heights, n->variants[v].fields, n->variants[v].nfields);
        if (h < *height) {
            *height = h;
            best = v;
        }
    }
    return best;
}

/* The arm of a Result that nests least, Ok on a tie */
static uint32_t least_arm(const uint32_t* heights, const GeasTypeNode* n) {
    return heights[n->kids[1]] < heights[n->kids[0]] ? 1 : 0;
}

/* One node's height from the current heights of its kids */
static uint32_t node_height(const uint32_t* heights, const GeasTypeNode* n) {
    uint32_t inner = 0;
    switch ((GeasTypeTag)n->tag) {
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
        inner = deepest(heights, n->kids, n->nkids);
        break;
    case GEAS_TY_RESULT:
        inner = heights[n->kids[least_arm(heights, n)]];
        break;
    case GEAS_TY_SUM:
        (void)least_variant(heights, n, &inner);
        break;
    default:
        /* A scalar, an empty List or Map, and a None stand alone */
        return 1;
    }
    return inner == HEIGHT_NONE ? HEIGHT_NONE : inner + 1;
}

GeasStatus geas_rt_type_heights(const GeasContractDesc* d, uint32_t** out) {
    *out = NULL;
    if (d->ntypes == 0) return GEAS_OK;
    uint32_t* heights = malloc((size_t)d->ntypes * sizeof(uint32_t));
    if (!heights) return GEAS_ERR_OOM;
    for (uint32_t i = 0; i < d->ntypes; i++) heights[i] = HEIGHT_NONE;
    /* Each pass settles at least one more node, so ntypes passes reach the
     * fixed point and the pass after it changes nothing */
    int is_changed = 1;
    for (uint32_t pass = 0; is_changed && pass <= d->ntypes; pass++) {
        is_changed = 0;
        for (uint32_t i = 0; i < d->ntypes; i++) {
            uint32_t h = node_height(heights, &d->types[i]);
            if (h < heights[i]) {
                heights[i] = h;
                is_changed = 1;
            }
        }
    }
    *out = heights;
    return GEAS_OK;
}

/* A node count not settled yet, and the saturated count of a smallest value
 * past the line */
#define NODES_UNKNOWN UINT32_MAX
#define NODES_PAST (GEAS_SLOT_MIN_NODES_MAX + 1u)

/* One more than the sum of a run of counts, saturating at NODES_PAST, or
 * NODES_UNKNOWN when any of them is not settled yet */
static uint32_t nodes_of(const uint32_t* counts, const uint32_t* kids, uint32_t n) {
    uint32_t total = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = counts[kids[i]];
        if (k == NODES_UNKNOWN) return NODES_UNKNOWN;
        total = k >= NODES_PAST - total ? NODES_PAST : total + k;
    }
    return total;
}

/* The node count of one node's smallest value from the counts of the kids
 * the height rule chose */
static uint32_t node_count(const uint32_t* heights, const uint32_t* counts,
                           const GeasTypeNode* n) {
    switch ((GeasTypeTag)n->tag) {
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
        return nodes_of(counts, n->kids, n->nkids);
    case GEAS_TY_RESULT:
        return nodes_of(counts, &n->kids[least_arm(heights, n)], 1);
    case GEAS_TY_SUM: {
        uint32_t height = 0;
        const GeasTypeVariant* v = &n->variants[least_variant(heights, n, &height)];
        return nodes_of(counts, v->fields, v->nfields);
    }
    default:
        return 1;
    }
}

/* The node counts of every smallest value, NODES_UNKNOWN for a type with no
 * finite value. heights is what geas_rt_type_heights settled */
static GeasStatus type_counts(const GeasContractDesc* d, const uint32_t* heights,
                              uint32_t** out) {
    *out = NULL;
    uint32_t* counts = malloc((size_t)d->ntypes * sizeof(uint32_t));
    if (!counts) return GEAS_ERR_OOM;
    for (uint32_t i = 0; i < d->ntypes; i++) counts[i] = NODES_UNKNOWN;
    /* A count needs only kids of lower height, so each pass settles at least
     * the lowest node still open, as for the heights */
    int is_changed = 1;
    for (uint32_t pass = 0; is_changed && pass <= d->ntypes; pass++) {
        is_changed = 0;
        for (uint32_t i = 0; i < d->ntypes; i++) {
            if (counts[i] != NODES_UNKNOWN || heights[i] == HEIGHT_NONE) continue;
            counts[i] = node_count(heights, counts, &d->types[i]);
            if (counts[i] != NODES_UNKNOWN) is_changed = 1;
        }
    }
    *out = counts;
    return GEAS_OK;
}

/* Every slot type in the table, nesting no deeper than GEAS_VALUE_MAX_DEPTH
 * and holding no more than GEAS_SLOT_MIN_NODES_MAX nodes at its smallest */
static GeasStatus check_slots(const GeasContractDesc* d, const uint32_t* heights,
                              const uint32_t* counts) {
    for (uint32_t s = 0; s < d->course_nslots; s++) {
        uint32_t ty = d->course_slot_types[s];
        if (ty == GEAS_NO_TYPE) continue;
        if (ty >= d->ntypes || heights[ty] > GEAS_VALUE_MAX_DEPTH) return GEAS_ERR_TYPE;
        if (counts[ty] > GEAS_SLOT_MIN_NODES_MAX) return GEAS_ERR_TYPE;
    }
    return GEAS_OK;
}

GeasStatus geas_rt_validate_slot_types(const GeasContractDesc* d) {
    if (!d->course_slot_types) return GEAS_OK;
    /* A slot table rides only a course with slots and a type table to index */
    if (!d->course_fn || d->course_nslots == 0 || d->ntypes == 0) return GEAS_ERR_TYPE;
    uint32_t* heights = NULL;
    uint32_t* counts = NULL;
    GeasStatus st = geas_rt_type_heights(d, &heights);
    if (st == GEAS_OK) st = type_counts(d, heights, &counts);
    if (st == GEAS_OK) st = check_slots(d, heights, counts);
    free(counts);
    free(heights);
    return st;
}

static GeasStatus zero_of(GeasContract* c, const uint32_t* heights, uint32_t ty,
                          GeasValue* out);

/* The slots of a tuple, a record, or a variant, each at its smallest */
static GeasStatus zero_slots(GeasContract* c, const uint32_t* heights, const uint32_t* kids,
                             uint32_t n, GeasValue* out) {
    if (n == 0) return GEAS_OK;
    GeasValue* slots = (GeasValue*)geas_bytes(c, (uint64_t)n * sizeof(GeasValue));
    if (!slots) return GEAS_ERR_OOM;
    out->as.list.data = slots;
    out->as.list.len = n;
    out->as.list.cap = n;
    for (uint32_t i = 0; i < n; i++) {
        GeasStatus st = zero_of(c, heights, kids[i], &slots[i]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* The arm of a Result that nests least, its payload boxed unless it is Unit */
static GeasStatus zero_result(GeasContract* c, const uint32_t* heights, const GeasTypeNode* n,
                              GeasValue* out) {
    uint32_t arm = least_arm(heights, n);
    uint32_t inner = n->kids[arm];
    out->tag = arm;
    if (c->desc->types[inner].tag == GEAS_TY_UNIT) return GEAS_OK;
    GeasValue* box = (GeasValue*)geas_bytes(c, sizeof(GeasValue));
    if (!box) return GEAS_ERR_OOM;
    out->as.box = box;
    return zero_of(c, heights, inner, box);
}

/* The smallest value of one type onto the instance heap. Every choice
 * follows a kid of lower height, so the walk ends within the type's height */
static GeasStatus zero_of(GeasContract* c, const uint32_t* heights, uint32_t ty,
                          GeasValue* out) {
    memset(out, 0, sizeof(*out));
    if (ty == GEAS_NO_TYPE) return GEAS_OK;
    const GeasTypeNode* n = &c->desc->types[ty];
    out->ty = n->tag;
    switch ((GeasTypeTag)n->tag) {
    case GEAS_TY_LIST:
    case GEAS_TY_MAP:
        out->as.list.elem_ty = c->desc->types[n->kids[0]].tag;
        return GEAS_OK;
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
        return zero_slots(c, heights, n->kids, n->nkids, out);
    case GEAS_TY_SUM: {
        uint32_t height = 0;
        uint32_t v = least_variant(heights, n, &height);
        out->tag = v;
        return zero_slots(c, heights, n->variants[v].fields, n->variants[v].nfields, out);
    }
    case GEAS_TY_RESULT:
        return zero_result(c, heights, n, out);
    default:
        /* Zero is a value of every scalar and the None of an Option */
        return GEAS_OK;
    }
}

GeasStatus geas_rt_course_slots_zero(GeasContract* c, GeasValue* slots, uint32_t nslots) {
    const GeasContractDesc* d = c->desc;
    if (!d->course_slot_types || nslots == 0) return GEAS_OK;
    uint32_t* heights = NULL;
    GeasStatus st = geas_rt_type_heights(d, &heights);
    for (uint32_t s = 0; st == GEAS_OK && s < nslots; s++) {
        st = zero_of(c, heights, d->course_slot_types[s], &slots[s]);
    }
    free(heights);
    return st;
}

uint32_t geas_rt_course_slot_type(const GeasContractDesc* d, uint32_t slot) {
    if (!d->course_slot_types || slot >= d->course_nslots) return GEAS_NO_TYPE;
    return d->course_slot_types[slot];
}
