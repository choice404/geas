/* amendrow.c: a park row read against the shape it was written under. An
 * amended resume reads a row version N wrote while the registered
 * descriptor is version M, so nothing here reads the descriptor's tables:
 * the counts come from the amends block's old name lists, and every column
 * is decoded into an image indexed the way N indexed it, which amend.c then
 * maps onto the new instance.
 *
 * The columns are the ones a plain resume reads: the vows back to back, one
 * latch byte per item, one presence byte and an optional Err payload per
 * item, one fate byte per subcontract, and the workflow blob of version 2
 * through 5, or none at all for a row from before the workflow state. The
 * checks are the ones a plain resume runs that need no descriptor: latch,
 * presence, and fate bytes in range, an open episode refused, a lapse only
 * on a broken latch, a due time only with attempts saved, a course state
 * the park writes, and nothing left over anywhere. A row whose counts
 * disagree with the old lists, or whose bytes do not decode, is
 * GEAS_ERR_TYPE.
 *
 * Values decode onto the new instance's heap, where they stay; the image's
 * arrays are plain heap, released by geas_rt_amend_image_free once the
 * mapping is done or abandoned. Runs on an instance nothing else can reach
 * yet, with no lock held. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

#define FLOW_MAGIC "GFLW"

/* ---- the cursor ---- */

/* A read past the end latches bad and answers zero, so a section checks
 * once at its end */
typedef struct RowIn {
    const uint8_t* buf;
    size_t         len;
    size_t         off;
    int            bad;
} RowIn;

static const uint8_t* in_take(RowIn* in, size_t n) {
    if (in->bad || in->len - in->off < n) {
        in->bad = 1;
        return NULL;
    }
    const uint8_t* at = in->buf + in->off;
    in->off += n;
    return at;
}

static uint8_t in_u8(RowIn* in) {
    const uint8_t* at = in_take(in, 1);
    return at ? at[0] : 0;
}

static uint32_t in_u32(RowIn* in) {
    const uint8_t* at = in_take(in, 4);
    uint32_t v = 0;
    for (int i = 0; at && i < 4; i++) v |= (uint32_t)at[i] << (8 * i);
    return v;
}

static uint64_t in_u64(RowIn* in) {
    const uint8_t* at = in_take(in, 8);
    uint64_t v = 0;
    for (int i = 0; at && i < 8; i++) v |= (uint64_t)at[i] << (8 * i);
    return v;
}

static int64_t in_i64(RowIn* in) {
    return (int64_t)in_u64(in);
}

/* One wire value onto the instance heap. A value that does not decode is a
 * row that disagrees with its shape, GEAS_ERR_TYPE, unless memory ran out */
static GeasStatus in_wire(RowIn* in, GeasContract* c, GeasValue* v) {
    if (in->bad) return GEAS_ERR_TYPE;
    size_t used = 0;
    GeasStatus st = geas_wire_decode_value(c, in->buf + in->off, in->len - in->off, v, &used);
    if (st == GEAS_ERR_OOM) return st;
    if (st != GEAS_OK) return GEAS_ERR_TYPE;
    in->off += used;
    return GEAS_OK;
}

/* A presence byte, then the value when present */
static GeasStatus in_value(RowIn* in, GeasContract* c, GeasValue* v, uint8_t* present_out) {
    uint8_t present = in_u8(in);
    if (in->bad || present > 1) return GEAS_ERR_TYPE;
    if (present_out) *present_out = present;
    if (!present) return GEAS_OK;
    return in_wire(in, c, v);
}

static RowIn row_in(GeasString s) {
    RowIn in = { s.ptr, (size_t)s.len, 0, 0 };
    return in;
}

/* ---- the image ---- */

/* n zeroed elements, NULL for none; a failure sets *is_oom */
static void* zalloc(size_t n, size_t size, int* is_oom) {
    if (n == 0) return NULL;
    void* p = calloc(n, size);
    if (!p) *is_oom = 1;
    return p;
}

static GeasStatus image_alloc(GeasAmendImage* img, const GeasAmendDesc* a) {
    int is_oom = 0;
    size_t items = a->nold_items;
    img->vows = zalloc(a->nold_vows, sizeof(GeasValue), &is_oom);
    img->errs = zalloc(items, sizeof(GeasValue), &is_oom);
    img->eligible_ms = zalloc(items, sizeof(int64_t), &is_oom);
    img->lapsed = zalloc(items, sizeof(uint8_t), &is_oom);
    img->latch_seq = zalloc(items, sizeof(uint32_t), &is_oom);
    img->ok = zalloc(items, sizeof(GeasValue), &is_oom);
    img->ok_set = zalloc(items, sizeof(uint8_t), &is_oom);
    img->retry_attempts = zalloc(items, sizeof(uint32_t), &is_oom);
    img->retry_due_ms = zalloc(items, sizeof(int64_t), &is_oom);
    img->sub_eligible_ms = zalloc(a->nold_subs, sizeof(int64_t), &is_oom);
    img->remedy_state = zalloc(a->nold_remedies, sizeof(uint8_t), &is_oom);
    img->remedy_status = zalloc(a->nold_remedies, sizeof(int32_t), &is_oom);
    img->remedy_lapsed = zalloc(a->nold_remedies, sizeof(uint8_t), &is_oom);
    img->remedy_err = zalloc(a->nold_remedies, sizeof(GeasValue), &is_oom);
    return is_oom ? GEAS_ERR_OOM : GEAS_OK;
}

void geas_rt_amend_image_free(GeasAmendImage* img) {
    free(img->vows);
    free(img->errs);
    free(img->eligible_ms);
    free(img->lapsed);
    free(img->latch_seq);
    free(img->ok);
    free(img->ok_set);
    free(img->retry_attempts);
    free(img->retry_due_ms);
    free(img->sub_eligible_ms);
    free(img->remedy_state);
    free(img->remedy_status);
    free(img->remedy_lapsed);
    free(img->remedy_err);
    free(img->frame.slots);
    free(img->join);
    memset(img, 0, sizeof(*img));
}

/* ---- the plain columns ---- */

/* N's vows back to back, exactly nold_vows of them */
static GeasStatus decode_vows(GeasContract* c, const GeasAmendDesc* a, GeasString b,
                              GeasAmendImage* img) {
    RowIn in = row_in(b);
    for (uint32_t i = 0; i < a->nold_vows; i++) {
        GeasStatus st = in_wire(&in, c, &img->vows[i]);
        if (st != GEAS_OK) return st;
    }
    return in.off == in.len ? GEAS_OK : GEAS_ERR_TYPE;
}

/* One latch byte per item, then one presence byte and an optional Err
 * payload per item */
static GeasStatus decode_latches(GeasContract* c, const GeasAmendDesc* a,
                                 const GeasParkRow* row, GeasAmendImage* img) {
    if (row->latches.len != a->nold_items) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < a->nold_items; i++) {
        if (row->latches.ptr[i] > PLEDGE_BROKEN) return GEAS_ERR_TYPE;
    }
    img->latches = row->latches.ptr;
    RowIn in = row_in(row->errs);
    for (uint32_t i = 0; i < a->nold_items; i++) {
        GeasStatus st = in_value(&in, c, &img->errs[i], NULL);
        if (st != GEAS_OK) return st;
    }
    return in.off == in.len ? GEAS_OK : GEAS_ERR_TYPE;
}

/* One fate byte per subcontract; an open episode never parks */
static GeasStatus decode_subtxn(const GeasAmendDesc* a, const GeasParkRow* row,
                                GeasAmendImage* img) {
    if (row->subtxn.len != a->nold_subs) return GEAS_ERR_TYPE;
    for (uint32_t s = 0; s < a->nold_subs; s++) {
        uint8_t fate = row->subtxn.ptr[s];
        if (fate == TXN_OPEN || fate > TXN_ROLLED_BACK) return GEAS_ERR_TYPE;
    }
    img->subtxn = row->subtxn.ptr;
    return GEAS_OK;
}

/* ---- the workflow blob ---- */

/* An eligibility stamp as a blob of this version wrote it: through version
 * 4 any negative stamp is the item not eligible yet, from version 5 that is
 * GEAS_NOT_ELIGIBLE and every other stamp is verbatim */
static int64_t read_stamp(int64_t stamp, uint32_t version) {
    if (version <= 4 && stamp < 0) return GEAS_NOT_ELIGIBLE;
    return stamp;
}

static GeasStatus decode_items(GeasContract* c, const GeasAmendDesc* a, RowIn* in,
                               uint32_t version, GeasAmendImage* img) {
    uint32_t n = in_u32(in);
    if (in->bad || n != a->nold_items) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < n; i++) {
        img->eligible_ms[i] = read_stamp(in_i64(in), version);
        img->lapsed[i] = in_u8(in);
        img->latch_seq[i] = in_u32(in);
        GeasStatus st = in_value(in, c, &img->ok[i], &img->ok_set[i]);
        if (st != GEAS_OK) return st;
        if (version >= 3) {
            img->retry_attempts[i] = in_u32(in);
            img->retry_due_ms[i] = in_i64(in);
        }
        if (img->lapsed[i] > 1) return GEAS_ERR_TYPE;
        if (img->lapsed[i] && img->latches[i] != PLEDGE_BROKEN) return GEAS_ERR_TYPE;
        if (!img->retry_attempts[i] && img->retry_due_ms[i]) return GEAS_ERR_TYPE;
    }
    return in->bad ? GEAS_ERR_TYPE : GEAS_OK;
}

/* A remedy recorded as running was in flight when the row was written; no
 * remedy runs twice, so it reads failed with a state error, the rule a
 * plain resume keeps */
static GeasStatus decode_remedies(GeasContract* c, const GeasAmendDesc* a, RowIn* in,
                                  GeasAmendImage* img) {
    uint32_t n = in_u32(in);
    if (in->bad || n != a->nold_remedies) return GEAS_ERR_TYPE;
    for (uint32_t r = 0; r < n; r++) {
        uint8_t state = in_u8(in);
        img->remedy_status[r] = (int32_t)in_u32(in);
        img->remedy_lapsed[r] = in_u8(in);
        GeasStatus st = in_value(in, c, &img->remedy_err[r], NULL);
        if (st != GEAS_OK) return st;
        if (state > REMEDY_FAILED || img->remedy_lapsed[r] > 1) return GEAS_ERR_TYPE;
        if (state == REMEDY_RUNNING) {
            state = REMEDY_FAILED;
            img->remedy_status[r] = (int32_t)GEAS_ERR_STATE;
        }
        img->remedy_state[r] = state;
    }
    return in->bad ? GEAS_ERR_TYPE : GEAS_OK;
}

/* The together in flight, its calls by N's item index */
static GeasStatus decode_join(GeasContract* c, const GeasAmendDesc* a, RowIn* in,
                              GeasAmendImage* img) {
    uint32_t step = in_u32(in);
    uint32_t n = in_u32(in);
    if (in->bad) return GEAS_ERR_TYPE;
    if (n == 0) return step == 0 ? GEAS_OK : GEAS_ERR_TYPE;
    /* Every call carries at least its item and its outcome byte */
    if (n > (in->len - in->off) / 5) return GEAS_ERR_TYPE;
    img->join = calloc(n, sizeof(GeasAmendCall));
    if (!img->join) return GEAS_ERR_OOM;
    img->join_step = step;
    img->njoin = n;
    for (uint32_t i = 0; i < n; i++) {
        GeasAmendCall* call = &img->join[i];
        call->item = in_u32(in);
        call->has_outcome = in_u8(in);
        if (in->bad || call->has_outcome > 1 || call->item >= a->nold_items) return GEAS_ERR_TYPE;
        if (!call->has_outcome) continue;
        call->status = (GeasStatus)in_u32(in);
        uint8_t present = 0;
        GeasStatus st = in_value(in, c, &call->value, &present);
        if (st != GEAS_OK) return st;
        if ((call->status == GEAS_OK) != (present == 1)) return GEAS_ERR_TYPE;
    }
    return in->bad ? GEAS_ERR_TYPE : GEAS_OK;
}

/* The course frame, its slot count read from the frame itself since N's
 * descriptor is not here; amend.c holds it against the registered one */
static GeasStatus decode_frame(GeasContract* c, const GeasAmendDesc* a, RowIn* in,
                               uint32_t version, GeasAmendImage* img) {
    uint8_t present = in_u8(in);
    if (in->bad || present > 1) return GEAS_ERR_TYPE;
    if (!present) return GEAS_OK;
    img->has_frame = 1;
    img->course_hash = in_u64(in);
    img->course_state = in_u8(in);
    img->frame.step = in_u32(in);
    img->frame.status = in_u32(in);
    img->frame.wait_due_ms = in_i64(in);
    img->frame.wait_step = in_u32(in);
    uint32_t nslots = in_u32(in);
    if (in->bad) return GEAS_ERR_TYPE;
    if (img->course_state < GEAS_COURSE_RUNNING || img->course_state > GEAS_COURSE_CANCELLED) {
        return GEAS_ERR_TYPE;
    }
    /* A clock below zero sets a deadline below zero from version 5 */
    if (version <= 4 && img->frame.wait_due_ms < 0) return GEAS_ERR_TYPE;
    /* Every slot carries at least its presence byte */
    if (nslots > in->len - in->off) return GEAS_ERR_TYPE;
    if (nslots > 0) {
        img->frame.slots = calloc(nslots, sizeof(GeasValue));
        if (!img->frame.slots) return GEAS_ERR_OOM;
    }
    img->frame.nslots = nslots;
    for (uint32_t i = 0; i < nslots; i++) {
        uint8_t is_present = 0;
        GeasStatus st = in_value(in, c, &img->frame.slots[i], &is_present);
        if (st != GEAS_OK) return st;
        if (!is_present) return GEAS_ERR_TYPE;
    }
    return decode_join(c, a, in, img);
}

static GeasStatus decode_flow(GeasContract* c, const GeasAmendDesc* a, GeasString b,
                              GeasAmendImage* img) {
    if (b.len == 0) return GEAS_OK;
    RowIn in = row_in(b);
    const uint8_t* magic = in_take(&in, 4);
    if (!magic || memcmp(magic, FLOW_MAGIC, 4) != 0) return GEAS_ERR_TYPE;
    uint32_t version = in_u32(&in);
    if (in.bad || version < 2 || version > 5) return GEAS_ERR_TYPE;
    img->has_flow = 1;
    img->signed_ms = in_i64(&in);
    img->term_lapsed = in_u8(&in);
    img->next_seq = in_u32(&in);
    if (in.bad || img->term_lapsed > 1) return GEAS_ERR_TYPE;
    GeasStatus st = decode_items(c, a, &in, version, img);
    if (st != GEAS_OK) return st;
    uint32_t nsubs = in_u32(&in);
    if (in.bad || nsubs != a->nold_subs) return GEAS_ERR_TYPE;
    for (uint32_t s = 0; s < nsubs; s++) img->sub_eligible_ms[s] = read_stamp(in_i64(&in), version);
    st = decode_remedies(c, a, &in, img);
    if (st != GEAS_OK) return st;
    if (version >= 4) st = decode_frame(c, a, &in, version, img);
    if (st != GEAS_OK) return st;
    return in.off == in.len ? GEAS_OK : GEAS_ERR_TYPE;
}

GeasStatus geas_rt_amend_decode(GeasContract* c, const GeasAmendDesc* a,
                                const GeasParkRow* row, GeasAmendImage* img) {
    memset(img, 0, sizeof(*img));
    GeasStatus st = image_alloc(img, a);
    if (st == GEAS_OK) st = decode_vows(c, a, row->vows, img);
    if (st == GEAS_OK) st = decode_latches(c, a, row, img);
    if (st == GEAS_OK) st = decode_subtxn(a, row, img);
    if (st == GEAS_OK) st = decode_flow(c, a, row->flow, img);
    return st;
}
