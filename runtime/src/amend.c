/* amend.c: a parked instance resumed as a later version. A contract at
 * version M carries one amends block per earlier version N it resumes, and
 * a park row written under N, whose version and shape hash match a block,
 * stands back up as an ordinary version M instance through it. Blocks do not
 * chain: each maps N straight to M.
 *
 * The resume builds a fresh instance for M exactly as a plain resume does,
 * then applies the block to it, all or nothing: amendrow.c decodes the row
 * against N's counts into an image, and this unit maps the image through the
 * block's tables. Every step either lands on the unpublished instance or
 * answers a status, and the caller discards the instance on any status, so a
 * failed resume leaves the row as it was and nothing half built behind.
 *
 * The order of the mapping is fixed:
 *
 *   1. the course frame is checked: a row frame whose hash is not the
 *      block's from_course_hash is GEAS_ERR_VERSION, and a frame kept for a
 *      course whose hash moved is GEAS_ERR_VERSION too;
 *   2. the vows: each of N's vows through vow_map, then M's defaults for the
 *      slots still empty, then vows_fn, then every slot accounted for;
 *   3. the contract's own fields: the signing time, the term lapse, and the
 *      latch counter;
 *   4. each of N's items through item_map: the latch, the Err payload, the
 *      eligibility stamp, the lapse, the latch order, the Ok value or signal
 *      payload, and the retry progress held to M's retry budget;
 *   5. each of N's subcontracts through sub_map: the transactional fate and
 *      the eligibility stamp;
 *   6. each of N's remedies through remedy_map: its state, status, lapse,
 *      and Err payload;
 *   7. the seeds of the items M adds;
 *   8. eligibility under M's terms;
 *   9. the course frame kept, dropped, or left for a fresh start.
 *
 * Whatever maps to -1 is dropped. The policy is evaluated once by the
 * caller, after the AMENDED and RESUMED events, under the instance lock.
 *
 * Locks. Everything here runs on an instance nothing else can reach yet,
 * with no lock held; the allocation helpers vows_fn may call take only the
 * fresh instance's own mutex. */

#include "runtime_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- the descriptor ---- */

/* n names, NULL entries allowed only where is_nullable says so */
static int names_ok(uint32_t n, const char* const* names, int is_nullable) {
    if (n == 0) return 1;
    if (!names) return 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!names[i] && !is_nullable) return 0;
    }
    return 1;
}

/* Every entry -1 or inside a table of nnew, and no index taken twice.
 * taken is scratch of nnew bytes, zeroed here */
static int map_ok(uint32_t nold, const int32_t* map, uint32_t nnew, uint8_t* taken) {
    if (nold == 0) return 1;
    if (!map) return 0;
    if (nnew) memset(taken, 0, nnew);
    for (uint32_t i = 0; i < nold; i++) {
        if (map[i] == -1) continue;
        if (map[i] < 0 || (uint32_t)map[i] >= nnew || taken[map[i]]) return 0;
        taken[map[i]] = 1;
    }
    return 1;
}

/* Every seed names an item no old item maps to, at most once, with a known
 * state. taken holds the item map's targets on entry */
static int seeds_ok(const GeasAmendDesc* a, uint32_t npledges, uint8_t* taken) {
    if (a->nlatches == 0) return 1;
    if (!a->latches) return 0;
    for (uint32_t l = 0; l < a->nlatches; l++) {
        const GeasAmendLatch* seed = &a->latches[l];
        if (seed->item >= npledges || seed->state > GEAS_AMEND_BROKEN) return 0;
        if (taken[seed->item]) return 0;
        taken[seed->item] = 1;
    }
    return 1;
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static int amend_ok(const GeasContractDesc* d, const GeasAmendDesc* a, uint8_t* taken) {
    if (a->from_version == d->version && a->from_shape_hash == d->shape_hash) return 0;
    if (!names_ok(a->nold_vows, a->old_vows, 0) || !names_ok(a->nold_items, a->old_items, 0) ||
        !names_ok(a->nold_subs, a->old_subs, 1) ||
        !names_ok(a->nold_remedies, a->old_remedies, 0)) {
        return 0;
    }
    if (!map_ok(a->nold_vows, a->vow_map, d->nvows, taken)) return 0;
    if (!map_ok(a->nold_subs, a->sub_map, d->nsubs, taken)) return 0;
    if (!map_ok(a->nold_remedies, a->remedy_map, d->nremedies, taken)) return 0;
    /* The item map last, its targets stay in taken for the seeds */
    if (!map_ok(a->nold_items, a->item_map, d->npledges, taken)) return 0;
    if (a->nold_items == 0 && d->npledges) memset(taken, 0, d->npledges);
    return seeds_ok(a, d->npledges, taken);
}

GeasStatus geas_rt_validate_amends(const GeasContractDesc* d) {
    if (d->namends == 0) return GEAS_OK;
    if (!d->amends) return GEAS_ERR_TYPE;
    uint32_t width = max_u32(max_u32(d->nvows, d->npledges), max_u32(d->nsubs, d->nremedies));
    uint8_t* taken = width ? malloc(width) : NULL;
    if (width && !taken) return GEAS_ERR_OOM;
    GeasStatus st = GEAS_OK;
    for (uint32_t i = 0; st == GEAS_OK && i < d->namends; i++) {
        const GeasAmendDesc* a = &d->amends[i];
        if (!amend_ok(d, a, taken)) st = GEAS_ERR_TYPE;
        /* Two blocks for one version and hash would make the resume guess */
        for (uint32_t j = 0; st == GEAS_OK && j < i; j++) {
            if (d->amends[j].from_version == a->from_version &&
                d->amends[j].from_shape_hash == a->from_shape_hash) {
                st = GEAS_ERR_TYPE;
            }
        }
    }
    free(taken);
    return st;
}

const GeasAmendDesc* geas_rt_amend_find(const GeasContractDesc* d, int64_t version,
                                        uint64_t hash) {
    for (uint32_t i = 0; i < d->namends; i++) {
        const GeasAmendDesc* a = &d->amends[i];
        if ((int64_t)a->from_version == version && a->from_shape_hash == hash) return a;
    }
    return NULL;
}

uint32_t geas_contract_amended_from(GeasContract* c) {
    return c ? c->amended_from : 0;
}

/* ---- the course check ---- */

/* Whether the row's frame is kept: the row has one, the block does not
 * restart the course, and M has a course to keep it in */
static int frame_kept(const GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img) {
    return img->has_frame && !a->restart_course && c->course;
}

/* Step 1. A frame written by a course other than the one the block names
 * is refused, and so is a frame kept for a course that changed, since only
 * restart_course gives a changed course a frame of its own. A kept frame
 * must fit M's slots and its together must name M's pledges */
static GeasStatus check_course(const GeasContract* c, const GeasAmendDesc* a,
                               const GeasAmendImage* img) {
    if (img->has_frame && img->course_hash != a->from_course_hash) return GEAS_ERR_VERSION;
    if (!frame_kept(c, a, img)) return GEAS_OK;
    if (c->desc->course_hash != img->course_hash) return GEAS_ERR_VERSION;
    if (img->frame.nslots != c->course->frame.nslots) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < img->njoin; i++) {
        int32_t j = a->item_map[img->join[i].item];
        if (j < 0 || geas_rt_is_signal(c->desc, (uint32_t)j)) return GEAS_ERR_TYPE;
    }
    return GEAS_OK;
}

/* ---- the vows ---- */

/* Copies a value onto the instance, deeply, refusing an instance handle the
 * way sign refuses one in a vow */
static GeasStatus vow_copy(GeasContract* c, const GeasValue* src, GeasValue* dst) {
    if (src->ty == GEAS_TY_INSTANCE) return GEAS_ERR_TYPE;
    GeasValue from = *src;
    return geas_value_deep_copy(c, &from, dst);
}

/* The carried vows, then M's defaults. is_set marks the slots filled */
static GeasStatus carry_vows(GeasContract* c, const GeasAmendDesc* a,
                             const GeasAmendImage* img, uint8_t* is_set) {
    const GeasContractDesc* d = c->desc;
    for (uint32_t i = 0; i < a->nold_vows; i++) {
        int32_t j = a->vow_map[i];
        if (j < 0) continue;
        if (img->vows[i].ty != d->vows[j].ty) return GEAS_ERR_TYPE;
        c->vow_vals[j] = img->vows[i];
        is_set[j] = 1;
    }
    for (uint32_t j = 0; j < d->nvows; j++) {
        if (is_set[j] || !d->vows[j].has_default) continue;
        GeasStatus st = vow_copy(c, &d->vows[j].default_value, &c->vow_vals[j]);
        if (st != GEAS_OK) return st;
        is_set[j] = 1;
    }
    return GEAS_OK;
}

/* vows_fn, then a deep copy of every slot it wrote, so nothing the instance
 * reads aliases the module or the caller after the resume. before is
 * scratch of nvows values */
static GeasStatus run_vows_fn(GeasContract* c, const GeasAmendDesc* a,
                              const GeasAmendImage* img, uint8_t* is_set, GeasValue* before) {
    uint32_t n = c->desc->nvows;
    memcpy(before, c->vow_vals, n * sizeof(GeasValue));
    GeasStatus st = a->vows_fn(c, img->vows, a->nold_vows, c->vow_vals, n);
    if (st != GEAS_OK) return st;
    for (uint32_t j = 0; j < n; j++) {
        if (memcmp(&before[j], &c->vow_vals[j], sizeof(GeasValue)) == 0) continue;
        GeasStatus copied = vow_copy(c, &c->vow_vals[j], &c->vow_vals[j]);
        if (copied != GEAS_OK) return copied;
        is_set[j] = 1;
    }
    return GEAS_OK;
}

/* Step 2. A slot nothing filled is GEAS_ERR_UNBOUND, a slot of another
 * type than M declares, at its top level or anywhere inside it, GEAS_ERR_TYPE */
static GeasStatus amend_vows(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img) {
    const GeasContractDesc* d = c->desc;
    if (d->nvows == 0) return GEAS_OK;
    c->vow_vals = (GeasValue*)geas_bytes(c, d->nvows * sizeof(GeasValue));
    uint8_t* is_set = calloc(d->nvows, 1);
    GeasValue* before = malloc(d->nvows * sizeof(GeasValue));
    GeasStatus st = (c->vow_vals && is_set && before) ? GEAS_OK : GEAS_ERR_OOM;
    if (c->vow_vals) memset(c->vow_vals, 0, d->nvows * sizeof(GeasValue));
    if (st == GEAS_OK) st = carry_vows(c, a, img, is_set);
    if (st == GEAS_OK && a->vows_fn) st = run_vows_fn(c, a, img, is_set, before);
    for (uint32_t j = 0; st == GEAS_OK && j < d->nvows; j++) {
        if (!is_set[j]) st = GEAS_ERR_UNBOUND;
        else if (c->vow_vals[j].ty != d->vows[j].ty) st = GEAS_ERR_TYPE;
        else st = geas_rt_check_vow(d, j, &c->vow_vals[j]);
    }
    free(before);
    free(is_set);
    return st;
}

/* ---- latches, subcontracts, remedies ---- */

/* Retry progress held to M's budget for item j: none when M gives the item
 * no retry, a signal included, and never more attempts than M allows. The
 * saved due time is kept, held to no later than now plus the delay M's term
 * gives the next attempt */
static void carry_retry(GeasContract* c, uint32_t j, uint32_t attempts, int64_t due_ms,
                        int64_t now) {
    const GeasTermDesc* t = geas_rt_pledge_term(c->desc, j);
    if (!attempts || !t || t->retry_count == 0 || geas_rt_is_signal(c->desc, j)) return;
    uint32_t kept = attempts < t->retry_count ? attempts : t->retry_count;
    int64_t bound = geas_rt_ms_after(now, geas_rt_retry_delay(t, kept - 1));
    c->flow.retry_attempts[j] = kept;
    c->flow.retry_due_ms[j] = due_ms < bound ? due_ms : bound;
}

/* Step 4 */
static void amend_items(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img,
                        int64_t now) {
    GeasFlow* f = &c->flow;
    for (uint32_t i = 0; i < a->nold_items; i++) {
        int32_t j = a->item_map[i];
        if (j < 0) continue;
        c->pledge_state[j] = img->latches[i];
        c->pledge_err[j] = img->errs[i];
        if (!img->has_flow) continue;
        f->pledge_eligible_ms[j] = img->eligible_ms[i];
        f->pledge_lapsed[j] = img->lapsed[i];
        f->latch_seq[j] = img->latch_seq[i];
        f->pledge_ok[j] = img->ok[i];
        f->pledge_ok_set[j] = img->ok_set[i];
        carry_retry(c, (uint32_t)j, img->retry_attempts[i], img->retry_due_ms[i], now);
    }
}

/* Step 5 */
static void amend_subs(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img) {
    for (uint32_t s = 0; s < a->nold_subs; s++) {
        int32_t t = a->sub_map[s];
        if (t < 0) continue;
        c->sub_txn[t] = img->subtxn[s];
        if (img->has_flow) c->flow.sub_eligible_ms[t] = img->sub_eligible_ms[s];
    }
}

/* Step 6. A row with no workflow blob has no remedy records, every remedy
 * reads idle */
static void amend_remedies(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img) {
    GeasFlow* f = &c->flow;
    if (!img->has_flow) return;
    for (uint32_t r = 0; r < a->nold_remedies; r++) {
        int32_t t = a->remedy_map[r];
        if (t < 0) continue;
        f->remedy_state[t] = img->remedy_state[r];
        f->remedy_status[t] = img->remedy_status[r];
        f->remedy_lapsed[t] = img->remedy_lapsed[r];
        f->remedy_err[t] = img->remedy_err[r];
    }
}

/* Step 7. A fulfilled seed latches after every carried latch and carries no
 * Ok value, so a remedy that takes one records GEAS_ERR_TYPE without its
 * body running, the rule for a row that kept no Ok value. A broken seed is
 * a broken latch with no payload and no lapse */
static void amend_seeds(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img) {
    for (uint32_t l = 0; l < a->nlatches; l++) {
        const GeasAmendLatch* seed = &a->latches[l];
        if (seed->state == GEAS_AMEND_FULFILLED) {
            c->pledge_state[seed->item] = PLEDGE_FULFILLED;
            if (img->has_flow) c->flow.latch_seq[seed->item] = ++c->flow.next_seq;
        } else if (seed->state == GEAS_AMEND_BROKEN) {
            c->pledge_state[seed->item] = PLEDGE_BROKEN;
        }
    }
}

/* Step 8. A row with no workflow blob rebuilds every stamp from the latches
 * the way a plain resume of such a row does, over M's terms */
static GeasStatus amend_eligibility(GeasContract* c, const GeasAmendImage* img, int64_t now) {
    if (!img->has_flow) return geas_rt_flow_decode(c, NULL, 0, now);
    geas_rt_recheck_eligible(c, now);
    return GEAS_OK;
}

/* ---- the course frame ---- */

/* Step 9. A kept frame enters M's course at its saved step with its slots,
 * a together in flight naming M's pledges. A dropped frame leaves M's course,
 * when there is one, to start at step 0 with the fresh frame course_init
 * built */
static GeasStatus amend_course(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img) {
    if (!frame_kept(c, a, img)) return GEAS_OK;
    GeasCourse* k = c->course;
    GeasCourseFrame* f = &k->frame;
    f->step = img->frame.step;
    f->status = img->frame.status;
    f->wait_due_ms = img->frame.wait_due_ms;
    f->wait_step = img->frame.wait_step;
    if (f->nslots) memcpy(f->slots, img->frame.slots, f->nslots * sizeof(GeasValue));
    k->state = img->course_state;
    k->restored = 1;
    if (img->njoin == 0) return GEAS_OK;
    GeasJoinCall* calls = calloc(img->njoin, sizeof(GeasJoinCall));
    if (!calls) return GEAS_ERR_OOM;
    k->join = calls;
    k->join_step = img->join_step;
    k->join_n = img->njoin;
    for (uint32_t i = 0; i < img->njoin; i++) {
        const GeasAmendCall* from = &img->join[i];
        calls[i].pledge = (uint32_t)a->item_map[from->item];
        if (!from->has_outcome) continue;
        calls[i].status = from->status;
        calls[i].value = from->value;
        calls[i].state = JOIN_DONE;
        k->join_done++;
    }
    return GEAS_OK;
}

/* ---- the apply ---- */

/* Steps 3 through 8, which cannot fail before the eligibility */
static GeasStatus amend_flow(GeasContract* c, const GeasAmendDesc* a, const GeasAmendImage* img,
                             int64_t now) {
    if (img->has_flow) {
        c->flow.signed_ms = img->signed_ms;
        c->flow.term_lapsed = img->term_lapsed;
        c->flow.next_seq = img->next_seq;
    }
    amend_items(c, a, img, now);
    amend_subs(c, a, img);
    amend_remedies(c, a, img);
    amend_seeds(c, a, img);
    return amend_eligibility(c, img, now);
}

GeasStatus geas_rt_amend_apply(GeasContract* c, const GeasAmendDesc* a, const GeasParkRow* row,
                               int64_t now) {
    GeasAmendImage img;
    GeasStatus st = geas_rt_amend_decode(c, a, row, &img);
    if (st == GEAS_OK) st = check_course(c, a, &img);
    if (st == GEAS_OK) st = amend_vows(c, a, &img);
    if (st == GEAS_OK) st = amend_flow(c, a, &img, now);
    if (st == GEAS_OK) st = amend_course(c, a, &img);
    if (st == GEAS_OK) c->amended_from = a->from_version;
    geas_rt_amend_image_free(&img);
    return st;
}
