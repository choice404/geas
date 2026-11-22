/* flowpark.c: the workflow state in the park row. The row's first ten
 * columns are what a parked instance has always been, the vows, the latches,
 * the Err payloads, the transactional fates. The eleventh, flow, is one blob
 * written and read here, and it carries what the terms, signals, and
 * remedies keep on an instance: the signing time in milliseconds, when each
 * item became eligible, which latches are lapses, the Ok value of every
 * fulfilled pledge and with it every signal payload, the latch order, and
 * where each remedy stands.
 *
 * The blob is versioned. It opens with the four bytes "GFLW" and a version
 * number, and this file reads versions 2 through 5. Version 3 adds the
 * attempts completed and the absolute retry due time for each pledge.
 * Version 4 adds the course frame at the tail: the course state, the step,
 * the status, the wait fields, the course hash, every slot, and the outcomes
 * a together in flight already has. Version 5 spells an item not eligible
 * yet as GEAS_NOT_ELIGIBLE instead of a negative stamp, so a clock that
 * reads below zero stamps like any other. Version 1 is
 * the row with no blob at all, the row every runtime before the workflow
 * state wrote, and an empty blob is how it reads here; such a row resumes
 * with the workflow state rebuilt from what the row does hold.
 *
 * Integers are little endian at fixed widths, values are the wire codec's
 * canonical encoding, the same bytes the vows and the Err payloads already
 * use in the row beside it. Everything runs under the instance lock, or on
 * an instance nothing else can reach yet. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

#define FLOW_MAGIC   "GFLW"
#define FLOW_VERSION 5u

/* The last version whose eligibility stamps spell "not eligible yet" as any
 * negative number; from version 5 the blob carries GEAS_NOT_ELIGIBLE itself
 * and every other stamp verbatim, a negative one from a clock below zero
 * included */
#define FLOW_NEGATIVE_UNSTAMPED 4u

/* ---- writing ---- */

/* A growable byte buffer. A failed growth latches oom and every later put is
 * a no-op, so the encoder checks once at the end. */
typedef struct FlowOut {
    uint8_t* buf;
    size_t   len;
    size_t   cap;
    int      oom;
} FlowOut;

static uint8_t* out_room(FlowOut* o, size_t n) {
    if (o->oom) return NULL;
    if (n > SIZE_MAX - o->len) {
        o->oom = 1;
        return NULL;
    }
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (cap < o->len + n) {
            if (cap > SIZE_MAX / 2) {
                o->oom = 1;
                return NULL;
            }
            cap *= 2;
        }
        uint8_t* grown = realloc(o->buf, cap);
        if (!grown) {
            o->oom = 1;
            return NULL;
        }
        o->buf = grown;
        o->cap = cap;
    }
    uint8_t* at = o->buf + o->len;
    o->len += n;
    return at;
}

static void out_bytes(FlowOut* o, const void* p, size_t n) {
    uint8_t* at = out_room(o, n);
    if (at) memcpy(at, p, n);
}

static void out_u8(FlowOut* o, uint8_t v) {
    out_bytes(o, &v, 1);
}

static void out_u32(FlowOut* o, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
    out_bytes(o, b, 4);
}

static void out_u64(FlowOut* o, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    out_bytes(o, b, 8);
}

static void out_i64(FlowOut* o, int64_t v) {
    out_u64(o, (uint64_t)v);
}

/* A presence byte, then the value's wire encoding when present. */
static GeasStatus out_value(FlowOut* o, const GeasValue* v, int present) {
    out_u8(o, (uint8_t)(present ? 1 : 0));
    if (!present) return GEAS_OK;
    size_t need = 0;
    /* The sizing call answers GEAS_ERR_OOM with *need set, the codec's own
     * size protocol, so only another status is a fault. */
    GeasStatus st = geas_wire_encode_value(v, NULL, 0, &need);
    if (st != GEAS_OK && st != GEAS_ERR_OOM) return st;
    /* No encoded value is empty: GEAS_ERR_OOM with no size is a value past
     * the node budget, which no resume could read back */
    if (need == 0) return GEAS_ERR_OOM;
    uint8_t* at = out_room(o, need);
    if (!at) return GEAS_ERR_OOM;
    return geas_wire_encode_value(v, at, need, &need);
}

static GeasStatus encode_pledges(const GeasContract* c, FlowOut* o) {
    const GeasFlow* f = &c->flow;
    uint32_t n = c->desc->npledges;
    out_u32(o, n);
    for (uint32_t i = 0; i < n; i++) {
        out_i64(o, f->pledge_eligible_ms[i]);
        out_u8(o, f->pledge_lapsed[i]);
        out_u32(o, f->latch_seq[i]);
        GeasStatus st = out_value(o, &f->pledge_ok[i], f->pledge_ok_set[i]);
        if (st != GEAS_OK) return st;
        out_u32(o, f->retry_attempts[i]);
        out_i64(o, f->retry_due_ms[i]);
    }
    return GEAS_OK;
}

static GeasStatus encode_remedies(const GeasContract* c, FlowOut* o) {
    const GeasFlow* f = &c->flow;
    static const GeasValue zero;
    uint32_t n = c->desc->nremedies;
    out_u32(o, n);
    for (uint32_t r = 0; r < n; r++) {
        out_u8(o, f->remedy_state[r]);
        out_u32(o, (uint32_t)f->remedy_status[r]);
        out_u8(o, f->remedy_lapsed[r]);
        int present = memcmp(&f->remedy_err[r], &zero, sizeof(zero)) != 0;
        GeasStatus st = out_value(o, &f->remedy_err[r], present);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* The calls of the together in flight: each one's pledge, and the outcome
 * of the ones that have it. A call still queued or running is written as
 * having none and runs again after a resume */
static GeasStatus encode_join(const GeasCourse* k, FlowOut* o) {
    uint32_t n = k->join ? k->join_n : 0;
    out_u32(o, n ? k->join_step : 0);
    out_u32(o, n);
    for (uint32_t i = 0; i < n; i++) {
        const GeasJoinCall* call = &k->join[i];
        out_u32(o, call->pledge);
        int has_outcome = call->state == JOIN_DONE;
        out_u8(o, (uint8_t)has_outcome);
        if (!has_outcome) continue;
        out_u32(o, (uint32_t)call->status);
        GeasStatus st = out_value(o, &call->value, call->status == GEAS_OK);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* The course frame, one presence byte first. A slot holding a value the
 * wire codec refuses, an instance handle, makes the park GEAS_ERR_TYPE */
static GeasStatus encode_course(const GeasContract* c, FlowOut* o) {
    const GeasCourse* k = c->course;
    out_u8(o, k ? 1 : 0);
    if (!k) return GEAS_OK;
    const GeasCourseFrame* f = &k->frame;
    out_u64(o, c->desc->course_hash);
    out_u8(o, (uint8_t)k->state);
    out_u32(o, f->step);
    out_u32(o, f->status);
    out_i64(o, f->wait_due_ms);
    out_u32(o, f->wait_step);
    out_u32(o, f->nslots);
    for (uint32_t i = 0; i < f->nslots; i++) {
        GeasStatus st = out_value(o, &f->slots[i], 1);
        if (st != GEAS_OK) return st;
    }
    return encode_join(k, o);
}

GeasStatus geas_rt_flow_encode(const GeasContract* c, uint8_t** buf_out,
                             size_t* len_out) {
    const GeasFlow* f = &c->flow;
    FlowOut o;
    memset(&o, 0, sizeof(o));
    out_bytes(&o, FLOW_MAGIC, 4);
    out_u32(&o, FLOW_VERSION);
    out_i64(&o, f->signed_ms);
    out_u8(&o, f->term_lapsed);
    out_u32(&o, f->next_seq);
    GeasStatus st = encode_pledges(c, &o);
    if (st == GEAS_OK) {
        out_u32(&o, c->desc->nsubs);
        for (uint32_t s = 0; s < c->desc->nsubs; s++) {
            out_i64(&o, f->sub_eligible_ms[s]);
        }
        st = encode_remedies(c, &o);
    }
    if (st == GEAS_OK) st = encode_course(c, &o);
    if (st == GEAS_OK && o.oom) st = GEAS_ERR_OOM;
    if (st != GEAS_OK) {
        free(o.buf);
        return st;
    }
    *buf_out = o.buf;
    *len_out = o.len;
    return GEAS_OK;
}

/* ---- reading ---- */

/* A cursor over the blob. A read past the end latches bad and answers zero,
 * so the decoder checks once per section. */
typedef struct FlowIn {
    const uint8_t* buf;
    size_t         len;
    size_t         off;
    int            bad;
} FlowIn;

static const uint8_t* in_take(FlowIn* in, size_t n) {
    if (in->bad || in->len - in->off < n) {
        in->bad = 1;
        return NULL;
    }
    const uint8_t* at = in->buf + in->off;
    in->off += n;
    return at;
}

static uint8_t in_u8(FlowIn* in) {
    const uint8_t* at = in_take(in, 1);
    return at ? at[0] : 0;
}

static uint32_t in_u32(FlowIn* in) {
    const uint8_t* at = in_take(in, 4);
    uint32_t v = 0;
    for (int i = 0; at && i < 4; i++) v |= (uint32_t)at[i] << (8 * i);
    return v;
}

static uint64_t in_u64(FlowIn* in) {
    const uint8_t* at = in_take(in, 8);
    uint64_t v = 0;
    for (int i = 0; at && i < 8; i++) v |= (uint64_t)at[i] << (8 * i);
    return v;
}

static int64_t in_i64(FlowIn* in) {
    return (int64_t)in_u64(in);
}

/* The mirror of out_value: decodes onto the instance heap when present.
 * *present_out, when non NULL, receives the presence byte. */
static GeasStatus in_value(FlowIn* in, GeasContract* c, GeasValue* v,
                          uint8_t* present_out) {
    uint8_t present = in_u8(in);
    if (in->bad || present > 1) return GEAS_ERR_TYPE;
    if (present_out) *present_out = present;
    if (!present) return GEAS_OK;
    size_t used = 0;
    GeasStatus st = geas_wire_decode_value(c, in->buf + in->off,
                                         in->len - in->off, v, &used);
    in->off += used;
    return st;
}

/* An eligibility stamp as a blob of this version wrote it */
static int64_t read_stamp(int64_t stamp, uint32_t version) {
    if (version <= FLOW_NEGATIVE_UNSTAMPED && stamp < 0) return GEAS_NOT_ELIGIBLE;
    return stamp;
}

/* The latest a saved retry may fall due: now plus the delay its term gives
 * the attempt the row says is next. A row cannot hold a pledge in its retry
 * wait for longer than the term itself would */
static int64_t retry_due_bound(const GeasTermDesc* t, uint32_t attempts, int64_t now) {
    return geas_rt_ms_after(now, geas_rt_retry_delay(t, attempts - 1));
}

static GeasStatus decode_pledges(GeasContract* c, FlowIn* in, uint32_t version,
                                 int64_t now) {
    GeasFlow* f = &c->flow;
    uint32_t n = in_u32(in);
    if (in->bad || n != c->desc->npledges) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < n; i++) {
        f->pledge_eligible_ms[i] = read_stamp(in_i64(in), version);
        f->pledge_lapsed[i] = in_u8(in);
        f->latch_seq[i] = in_u32(in);
        GeasStatus st = in_value(in, c, &f->pledge_ok[i], &f->pledge_ok_set[i]);
        if (st != GEAS_OK) return st;
        if (version >= 3) {
            f->retry_attempts[i] = in_u32(in);
            f->retry_due_ms[i] = in_i64(in);
            const GeasTermDesc* terms = geas_rt_pledge_term(c->desc, i);
            if (f->retry_attempts[i] &&
                (!terms || f->retry_attempts[i] > terms->retry_count ||
                 geas_rt_is_signal(c->desc, i))) return GEAS_ERR_TYPE;
            if (!f->retry_attempts[i] && f->retry_due_ms[i]) return GEAS_ERR_TYPE;
            if (f->retry_attempts[i]) {
                int64_t bound = retry_due_bound(terms, f->retry_attempts[i], now);
                if (f->retry_due_ms[i] > bound) f->retry_due_ms[i] = bound;
            }
        }
        /* A lapse is a broken latch and nothing else. */
        if (f->pledge_lapsed[i] > 1) return GEAS_ERR_TYPE;
        if (f->pledge_lapsed[i] && c->pledge_state[i] != PLEDGE_BROKEN) {
            return GEAS_ERR_TYPE;
        }
    }
    return in->bad ? GEAS_ERR_TYPE : GEAS_OK;
}

/* A remedy recorded as running was in flight when the park was written. Its
 * body may or may not have finished, and the rule is that no remedy runs
 * twice, so it resumes as failed with a state error and the host reads it
 * off the partial surface. */
static GeasStatus decode_remedies(GeasContract* c, FlowIn* in) {
    GeasFlow* f = &c->flow;
    uint32_t n = in_u32(in);
    if (in->bad || n != c->desc->nremedies) return GEAS_ERR_TYPE;
    for (uint32_t r = 0; r < n; r++) {
        uint8_t state = in_u8(in);
        f->remedy_status[r] = (int32_t)in_u32(in);
        f->remedy_lapsed[r] = in_u8(in);
        GeasStatus st = in_value(in, c, &f->remedy_err[r], NULL);
        if (st != GEAS_OK) return st;
        if (state > REMEDY_FAILED || f->remedy_lapsed[r] > 1) return GEAS_ERR_TYPE;
        if (state == REMEDY_RUNNING) {
            state = REMEDY_FAILED;
            f->remedy_status[r] = (int32_t)GEAS_ERR_STATE;
        }
        f->remedy_state[r] = state;
    }
    return in->bad ? GEAS_ERR_TYPE : GEAS_OK;
}

/* A row from before the workflow state. What it holds is the latches and
 * the signing second, and the rest is rebuilt from them: the second stands
 * for the millisecond stamp; an item that waits on nothing was eligible at
 * sign and is stamped so, its deadline counting from the one time the row
 * kept; an item whose after has landed is stamped with the resume, the
 * first moment this runtime can vouch for; the fulfilled latches are
 * ordered by declaration, the only order the row can offer. No latch reads
 * as a lapse and no Ok value exists. */
static GeasStatus rebuild_from_latches(GeasContract* c, int64_t now) {
    const GeasContractDesc* d = c->desc;
    GeasFlow* f = &c->flow;
    /* A signing second no clock could have produced is a corrupt row. */
    if (c->signed_at > INT64_MAX / 1000 || c->signed_at < INT64_MIN / 1000) {
        return GEAS_ERR_TYPE;
    }
    f->signed_ms = c->signed_at * 1000;
    geas_rt_refresh_eligible(c, now);
    for (uint32_t s = 0; s < d->nsubs; s++) {
        const GeasTermDesc* t = geas_rt_sub_term(d, s);
        if (!t || t->nafter == 0) f->sub_eligible_ms[s] = f->signed_ms;
    }
    for (uint32_t i = 0; i < d->npledges; i++) {
        const GeasTermDesc* t = geas_rt_pledge_term(d, i);
        if (t && t->nafter > 0) continue;
        if (f->pledge_eligible_ms[i] == GEAS_NOT_ELIGIBLE) continue;
        f->pledge_eligible_ms[i] = geas_rt_pledge_is_loose(d, i)
            ? f->signed_ms
            : f->sub_eligible_ms[d->pledges[i].sub];
    }
    for (uint32_t i = 0; i < d->npledges; i++) {
        if (c->pledge_state[i] == PLEDGE_FULFILLED) f->latch_seq[i] = ++f->next_seq;
    }
    return GEAS_OK;
}

/* The calls of a together the row caught in flight. A call with its outcome
 * comes back done; one without comes back idle and starts again when the
 * course polls the together at its step. The outcome's value decodes onto
 * the instance heap like any pledge value */
static GeasStatus decode_join(GeasContract* c, FlowIn* in) {
    GeasCourse* k = c->course;
    uint32_t step = in_u32(in);
    uint32_t n = in_u32(in);
    if (in->bad) return GEAS_ERR_TYPE;
    if (n == 0) return step == 0 ? GEAS_OK : GEAS_ERR_TYPE;
    /* Every call carries at least its pledge and its outcome byte */
    if (n > (in->len - in->off) / 5) return GEAS_ERR_TYPE;
    GeasJoinCall* calls = calloc(n, sizeof(GeasJoinCall));
    if (!calls) return GEAS_ERR_OOM;
    k->join = calls;
    k->join_step = step;
    k->join_n = n;
    for (uint32_t i = 0; i < n; i++) {
        GeasJoinCall* call = &calls[i];
        call->pledge = in_u32(in);
        uint8_t has_outcome = in_u8(in);
        if (in->bad || has_outcome > 1) return GEAS_ERR_TYPE;
        if (call->pledge >= c->desc->npledges ||
            geas_rt_is_signal(c->desc, call->pledge)) return GEAS_ERR_TYPE;
        if (!has_outcome) continue;
        call->status = (GeasStatus)in_u32(in);
        uint8_t present = 0;
        GeasStatus st = in_value(in, c, &call->value, &present);
        if (st != GEAS_OK) return st;
        if ((call->status == GEAS_OK) != (present == 1)) return GEAS_ERR_TYPE;
        call->state = JOIN_DONE;
        k->join_done++;
    }
    return in->bad ? GEAS_ERR_TYPE : GEAS_OK;
}

/* The course frame. A frame written by a course other than the registered
 * one, or for a contract that has no course now, is GEAS_ERR_VERSION; a
 * frame that disagrees with the descriptor in any other way is
 * GEAS_ERR_TYPE. A row with no frame leaves the course at step 0 */
static GeasStatus decode_course(GeasContract* c, FlowIn* in, uint32_t version) {
    uint8_t present = in_u8(in);
    if (in->bad || present > 1) return GEAS_ERR_TYPE;
    if (!present) return GEAS_OK;
    uint64_t hash = in_u64(in);
    if (in->bad) return GEAS_ERR_TYPE;
    GeasCourse* k = c->course;
    if (!k || hash != c->desc->course_hash) return GEAS_ERR_VERSION;
    GeasCourseFrame* f = &k->frame;
    uint8_t state = in_u8(in);
    f->step = in_u32(in);
    f->status = in_u32(in);
    f->wait_due_ms = in_i64(in);
    f->wait_step = in_u32(in);
    uint32_t nslots = in_u32(in);
    if (in->bad || nslots != f->nslots) return GEAS_ERR_TYPE;
    if (state < GEAS_COURSE_RUNNING || state > GEAS_COURSE_CANCELLED) return GEAS_ERR_TYPE;
    /* A deadline below zero is one a clock below zero set, from version 5.
     * However far off it reads, the course's first poll after the resume
     * holds it to the duration the course function passes again */
    if (version <= FLOW_NEGATIVE_UNSTAMPED && f->wait_due_ms < 0) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < nslots; i++) {
        uint8_t is_present = 0;
        GeasStatus st = in_value(in, c, &f->slots[i], &is_present);
        if (st != GEAS_OK) return st;
        if (!is_present) return GEAS_ERR_TYPE;
    }
    k->state = state;
    k->restored = 1;
    return decode_join(c, in);
}

GeasStatus geas_rt_flow_decode(GeasContract* c, const uint8_t* buf, size_t len,
                             int64_t now) {
    if (len == 0) return rebuild_from_latches(c, now);
    FlowIn in = { buf, len, 0, 0 };
    const uint8_t* magic = in_take(&in, 4);
    if (!magic || memcmp(magic, FLOW_MAGIC, 4) != 0) return GEAS_ERR_TYPE;
    uint32_t version = in_u32(&in);
    if (version < 2 || version > FLOW_VERSION || in.bad) return GEAS_ERR_TYPE;
    GeasFlow* f = &c->flow;
    f->signed_ms = in_i64(&in);
    f->term_lapsed = in_u8(&in);
    f->next_seq = in_u32(&in);
    if (in.bad || f->term_lapsed > 1) return GEAS_ERR_TYPE;
    GeasStatus st = decode_pledges(c, &in, version, now);
    if (st != GEAS_OK) return st;
    uint32_t nsubs = in_u32(&in);
    if (in.bad || nsubs != c->desc->nsubs) return GEAS_ERR_TYPE;
    for (uint32_t s = 0; s < nsubs; s++) f->sub_eligible_ms[s] = read_stamp(in_i64(&in), version);
    st = decode_remedies(c, &in);
    if (st != GEAS_OK) return st;
    if (version >= 4) st = decode_course(c, &in, version);
    if (st != GEAS_OK) return st;
    /* The blob must hold exactly what the descriptor declares. */
    return in.off == len ? GEAS_OK : GEAS_ERR_TYPE;
}
