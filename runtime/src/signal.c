/* signal.c: signals. A signal is an outside event a host delivers into a
 * live instance: a webhook, an approval, a delivery scan. It is an entry of
 * the pledges table marked GEAS_KIND_SIGNAL, so the latch, the requirements
 * atom, the item on the partial surface, and the after and within terms are
 * the pledge's own machinery and nothing here repeats them. What is
 * different is how it lands: no body runs and no future exists, the delivery
 * latches the signal fulfilled on the calling thread under the instance
 * lock, stores the payload where a pledge keeps its Ok value, and evaluates
 * the policy before it returns.
 *
 * The first delivery wins. An event that arrives twice, the retried webhook,
 * is accepted both times and changes nothing the second, so a host need not
 * deduplicate before it calls. */

#include "runtime_internal.h"

#include <string.h>

/* Builds the payload on the instance: Unit for no arguments, the value for
 * one, a Tuple for several, every argument deep copied so nothing the host
 * owns outlives the call. */
static GeasStatus build_payload(GeasContract* c, const GeasValue* args,
                               size_t nargs, GeasValue* out) {
    memset(out, 0, sizeof(*out));
    if (nargs == 0) return GEAS_OK;
    if (nargs == 1) return geas_value_deep_copy(c, &args[0], out);
    GeasStatus st = geas_tuple_new(c, nargs, out);
    if (st != GEAS_OK) return st;
    GeasValue* slots = (GeasValue*)out->as.list.data;
    for (size_t i = 0; i < nargs; i++) {
        st = geas_value_deep_copy(c, &args[i], &slots[i]);
        if (st != GEAS_OK) return st;
    }
    return GEAS_OK;
}

/* The checks a delivery passes, in the order the header promises them.
 * *pidx receives the signal's index on GEAS_OK. Under the instance lock. */
static GeasStatus signal_gate(GeasContract* c, const char* name, size_t nargs,
                             uint32_t* pidx) {
    if (!geas_rt_live(c)) return GEAS_ERR_STATE;
    const GeasPledgeDesc* p = geas_rt_find_pledge(c->desc, name);
    if (!p) return GEAS_ERR_NAME;
    uint32_t i = (uint32_t)(p - c->desc->pledges);
    if (!geas_rt_is_signal(c->desc, i)) return GEAS_ERR_TYPE;
    if (nargs != p->nargs) return GEAS_ERR_TYPE;
    if (c->flow.pledge_eligible_ms[i] < 0) return GEAS_ERR_STATE;
    if (c->flow.pledge_lapsed[i]) return GEAS_ERR_STATE;
    *pidx = i;
    return GEAS_OK;
}

GeasStatus geas_signal(GeasContract* c, const char* name,
                     const GeasValue* args, size_t nargs) {
    if (!c || !name) return GEAS_ERR_TYPE;
    if (nargs > 0 && !args) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    uint32_t pidx = 0;
    GeasStatus st = signal_gate(c, name, nargs, &pidx);
    /* Already delivered: the latch and the payload are the first one's. */
    if (st == GEAS_OK && c->pledge_state[pidx] == PLEDGE_PENDING) {
        GeasValue payload;
        st = build_payload(c, args, nargs, &payload);
        if (st == GEAS_OK) {
            c->pledge_state[pidx] = PLEDGE_FULFILLED;
            geas_rt_latched(c, pidx, &payload);
            geas_rt_eval_policy(c);
            geas_rt_remedy_drive(c);
        }
    }
    pthread_mutex_unlock(&c->mu);
    return st;
}

GeasStatus geas_signal_payload(GeasContract* c, const char* name,
                             const GeasValue** out) {
    if (!c || !name || !out) return GEAS_ERR_TYPE;
    pthread_mutex_lock(&c->mu);
    geas_rt_tick(c);
    GeasStatus st = GEAS_OK;
    const GeasPledgeDesc* p = geas_rt_find_pledge(c->desc, name);
    uint32_t i = p ? (uint32_t)(p - c->desc->pledges) : 0;
    if (!p) st = GEAS_ERR_NAME;
    else if (!geas_rt_is_signal(c->desc, i)) st = GEAS_ERR_TYPE;
    else if (!c->flow.pledge_ok_set[i]) st = GEAS_ERR_STATE;
    else *out = &c->flow.pledge_ok[i];
    pthread_mutex_unlock(&c->mu);
    return st;
}
