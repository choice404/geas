/* coursepark.c: autopark, the park row an instance keeps writing on its own.
 *
 * A host arms an instance with a store and a key. From then on every change
 * that matters to a resume marks the instance, a latch, a contract state, a
 * remedy starting or ending, a retry saved, a course landing or finishing a
 * together, and the row is written at the next durable point: the outermost
 * release of the instance lock on any thread, a course checkpoint after a
 * step, a course landing, and the moment before a remedy body runs. A row
 * written that way is the row geas_instance_park writes, under the same
 * rules, so a moment those rules refuse, a pledge body running or a receipt
 * nobody waited, leaves the change marked for the next durable point.
 *
 * With the course writing at every step boundary and at every landing, a
 * row read after a crash is never older than the last step that finished.
 * A write that fails is held on the course: its next operation answers the
 * failed status and the course halts on it, and a landing that would leave
 * the course waiting halts it there instead. A host driven instance reports
 * a failed write on stderr and writes again at its next change.
 *
 * A published release holds every write back. The release writes the last
 * row itself, with a course its cancel cut recorded as still running, so the
 * instance that claims the row enters that course where it stood.
 *
 * Locks. Everything runs under the instance lock. The write opens, fills,
 * and closes the store while holding it, the same hold a host park takes,
 * and flushing keeps the releases inside that write from writing again. */

#include "runtime_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A plain heap copy of a C string, NULL when out of memory */
static char* copy_cstr(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

GeasStatus geas_contract_set_autopark(GeasContract* c, const char* dsn, const char* key) {
    if (!c) return GEAS_ERR_TYPE;
    if (dsn && !key) return GEAS_ERR_TYPE;
    char* d = NULL;
    char* k = NULL;
    if (dsn) {
        d = copy_cstr(dsn);
        k = copy_cstr(key);
        if (!d || !k) {
            free(d);
            free(k);
            return GEAS_ERR_OOM;
        }
    }
    geas_rt_lock(c);
    /* A host thread waits out a delivery that would refuse the first row,
     * so the row is written at this call's own release */
    geas_rt_await_delivery(c, GEAS_AWAIT_PARK);
    free(c->autopark_dsn);
    free(c->autopark_key);
    c->autopark_dsn = d;
    c->autopark_key = k;
    c->autopark_dirty = d != NULL;
    /* A new store, or none, starts with no failure held against it */
    if (c->course) c->course->held = GEAS_OK;
    /* The outermost release writes the first row when the instance can be
     * parked as it stands */
    geas_rt_unlock(c);
    return GEAS_OK;
}

GeasStatus geas_rt_autopark_flush(GeasContract* c, int* is_deferred) {
    if (is_deferred) *is_deferred = 0;
    if (!c->autopark_dsn || !c->autopark_dirty || c->autopark_flushing) return GEAS_OK;
    /* Once a release is published the row is the release's to write: a
     * course its cancel cut must not reach the row as cancelled, and the
     * release writes the last row itself before the heap goes */
    if (geas_rt_release_published(c) || geas_rt_park_ready(c) != GEAS_OK) {
        if (is_deferred) *is_deferred = 1;
        return GEAS_OK;
    }
    c->autopark_dirty = 0;
    c->autopark_flushing = 1;
    GeasStatus st = geas_rt_park_write(c, c->autopark_dsn, c->autopark_key);
    c->autopark_flushing = 0;
    if (st == GEAS_OK) return GEAS_OK;
    fprintf(stderr, "[libgeasrt] autopark write for %s failed: %d\n", c->desc->name, st);
    if (c->course && c->course->held == GEAS_OK) c->course->held = st;
    return st;
}

void geas_rt_autopark_unlocking(GeasContract* c) {
    /* A failed write is reported and held inside the flush */
    (void)geas_rt_autopark_flush(c, NULL);
}

void geas_rt_autopark_free(GeasContract* c) {
    free(c->autopark_dsn);
    free(c->autopark_key);
    c->autopark_dsn = NULL;
    c->autopark_key = NULL;
}

/* The step boundary. The emitted code stores the next step and calls this,
 * so the step is published to the readers at once and the row it writes
 * enters the course after the step that just finished. A delivery another
 * thread holds is waited out before the write, with the lock given up for
 * the wait, so the row is written here and not deferred past it. A break
 * that landed during that wait is the cut, answered here as at an
 * operation, since the emitted code reads the slots right after this call
 * and a reclaim takes them */
GeasStatus geas_course_checkpoint(void* ctx, GeasCourseFrame* f) {
    if (!ctx || !f) return GEAS_ERR_TYPE;
    GeasContract* c = ctx;
    GeasCourse* k = geas_rt_course_of(c, f);
    if (!k) return GEAS_ERR_STATE;
    geas_rt_course_publish_step(k);
    if (k->held != GEAS_OK) return k->held;
    if (!c->autopark_dsn) return GEAS_OK;
    if (geas_rt_settle_delivery(c) && geas_rt_course_cut(c)) {
        k->refused = 1;
        return GEAS_ERR_STATE;
    }
    if (!c->autopark_dsn) return GEAS_OK;
    geas_rt_autopark_mark(c);
    return geas_rt_autopark_flush(c, NULL);
}
