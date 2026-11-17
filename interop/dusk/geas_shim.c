/* The C half of the dusk binding to libgeasrt. It holds only what a dusk
 * foreign block cannot say by itself: the union inside GeasValue, the two
 * trampolines the runtime calls back through, and the NULLs a few calls take.
 * Every other runtime call is declared straight in the dusk modules and links
 * against libgeasrt by its SONAME.
 *
 * Handles cross as int64 bit patterns, the opaque handle convention std.fs
 * uses, since dusk cannot compare a pointer against NULL.
 *
 * A pledge body or a remedy is bound with geas_pledge_bind_user. The user
 * pointer is a GdBinding holding the dusk dispatch function and a byte copy of
 * the dusk side slot, the state pointer and the handler. The runtime calls
 * gd_pledge_tramp on a pool worker, which hands the bytes back to dusk on that
 * same thread. The clock works the same way through gd_now and gd_sleep. Every
 * binding and clock stays alive until gd_runtime_release, after the runtime
 * itself is shut down, so a rebind never frees one an instance still holds.
 *
 * Values a host builds here live on the C heap, built by the gd_set_* calls
 * and freed by gd_values_free. The runtime deep copies them at sign, fulfill,
 * and signal, and a body's answer is deep copied onto its instance before the
 * host tree is freed. */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/include/geas/geas.h"

_Static_assert(sizeof(GeasValue) == 40, "GeasValue is 40 bytes");
_Static_assert(sizeof(GeasVowBinding) == 48, "GeasVowBinding is 48 bytes");
_Static_assert(sizeof(GeasAttrInfo) == 32, "GeasAttrInfo is 32 bytes");

/* ---- handles ---- */

static GeasValue* value_of(int64_t h) {
    return (GeasValue*)(intptr_t)h;
}

static int64_t handle_of(const void* p) {
    return (int64_t)(intptr_t)p;
}

/* Copies n bytes, the way a dusk slot travels through a void pointer */
void gd_copy(void* dst, const void* src, int64_t n) {
    memcpy(dst, src, (size_t)n);
}

/* The length of a C string the runtime handed out, 0 for NULL */
int64_t gd_cstr_len(int64_t p) {
    if (p == 0) {
        return 0;
    }
    return (int64_t)strlen((const char*)(intptr_t)p);
}

/* Copies a C string into buf with its NUL, answers its length or -1 when it does not fit */
int64_t gd_cstr_copy(int64_t p, char* buf, int64_t cap) {
    int64_t len = gd_cstr_len(p);
    if (len + 1 > cap) {
        return -1;
    }
    if (len > 0) {
        memcpy(buf, (const char*)(intptr_t)p, (size_t)len);
    }
    buf[len] = '\0';
    return len;
}

/* ---- reading a value ---- */

int64_t gd_ty(int64_t v) { return value_of(v)->ty; }
int64_t gd_tag(int64_t v) { return value_of(v)->tag; }
int64_t gd_int(int64_t v) { return value_of(v)->as.i; }
uint64_t gd_uint(int64_t v) { return value_of(v)->as.u; }
double gd_float(int64_t v) { return value_of(v)->as.f; }
int64_t gd_small(int64_t v) { return value_of(v)->as.b; }
int64_t gd_char(int64_t v) { return value_of(v)->as.ch; }
int64_t gd_box(int64_t v) { return handle_of(value_of(v)->as.box); }
int64_t gd_elem_ty(int64_t v) { return value_of(v)->as.list.elem_ty; }

/* The byte length of a String or Bytes, the element count of a list shaped value */
int64_t gd_len(int64_t v) {
    GeasValue* value = value_of(v);
    if (value->ty == GEAS_TY_STRING || value->ty == GEAS_TY_BYTES) {
        return (int64_t)value->as.s.len;
    }
    return (int64_t)value->as.list.len;
}

/* The i-th element of a list, tuple, map, record, or variant payload */
int64_t gd_elem(int64_t v, int64_t i) {
    GeasValue* data = (GeasValue*)value_of(v)->as.list.data;
    return handle_of(&data[i]);
}

/* One byte of a String or Bytes value */
int64_t gd_byte_at(int64_t v, int64_t i) {
    return value_of(v)->as.s.ptr[i];
}

/* Copies a String into buf with a NUL. Answers -1 when buf is short and -2
 * when the text holds a NUL byte, which a dusk string cannot carry */
int64_t gd_text_copy(int64_t v, char* buf, int64_t cap) {
    GeasValue* value = value_of(v);
    int64_t len = (int64_t)value->as.s.len;
    if (len + 1 > cap) {
        return -1;
    }
    if (len > 0 && memchr(value->as.s.ptr, 0, (size_t)len) != NULL) {
        return -2;
    }
    if (len > 0) {
        memcpy(buf, value->as.s.ptr, (size_t)len);
    }
    buf[len] = '\0';
    return len;
}

/* ---- building a value on the C heap ---- */

/* An array of n zeroed values, Unit each, never NULL on success, 0 when out of memory */
int64_t gd_values_new(int64_t n) {
    size_t count = n > 0 ? (size_t)n : 1;
    return handle_of(calloc(count, sizeof(GeasValue)));
}

int64_t gd_value_at(int64_t arr, int64_t i) {
    return handle_of(&value_of(arr)[i]);
}

static void tree_free(GeasValue* v);

/* Frees what a host built value owns, never the value struct itself */
static void tree_free(GeasValue* v) {
    switch (v->ty) {
    case GEAS_TY_STRING:
    case GEAS_TY_BYTES:
        free(v->as.s.ptr);
        break;
    case GEAS_TY_OPTION:
    case GEAS_TY_RESULT:
        if (v->as.box != NULL) {
            tree_free((GeasValue*)v->as.box);
            free(v->as.box);
        }
        break;
    case GEAS_TY_LIST:
    case GEAS_TY_MAP:
    case GEAS_TY_TUPLE:
    case GEAS_TY_RECORD:
    case GEAS_TY_SUM: {
        GeasValue* data = (GeasValue*)v->as.list.data;
        for (uint64_t i = 0; i < v->as.list.len; i++) {
            tree_free(&data[i]);
        }
        free(data);
        break;
    }
    default:
        break;
    }
    memset(v, 0, sizeof(*v));
}

/* Frees an array of n host built values and everything they own */
void gd_values_free(int64_t arr, int64_t n) {
    GeasValue* values = value_of(arr);
    if (values == NULL) {
        return;
    }
    for (int64_t i = 0; i < n; i++) {
        tree_free(&values[i]);
    }
    free(values);
}

/* Frees an out slot whose contents the runtime owns, never what it points at */
void gd_slot_free(int64_t slot) {
    free(value_of(slot));
}

static void set_scalar(GeasValue* v, uint32_t ty) {
    memset(v, 0, sizeof(*v));
    v->ty = ty;
}

void gd_set_unit(int64_t v) { set_scalar(value_of(v), GEAS_TY_UNIT); }

void gd_set_int(int64_t v, int64_t i) {
    set_scalar(value_of(v), GEAS_TY_INT);
    value_of(v)->as.i = i;
}

void gd_set_uint(int64_t v, uint64_t u) {
    set_scalar(value_of(v), GEAS_TY_UINT);
    value_of(v)->as.u = u;
}

void gd_set_float(int64_t v, double f) {
    set_scalar(value_of(v), GEAS_TY_FLOAT);
    value_of(v)->as.f = f;
}

void gd_set_bool(int64_t v, int64_t b) {
    set_scalar(value_of(v), GEAS_TY_BOOL);
    value_of(v)->as.b = b != 0;
}

void gd_set_byte(int64_t v, int64_t b) {
    set_scalar(value_of(v), GEAS_TY_BYTE);
    value_of(v)->as.b = (uint8_t)b;
}

void gd_set_char(int64_t v, int64_t ch) {
    set_scalar(value_of(v), GEAS_TY_CHAR);
    value_of(v)->as.ch = (uint32_t)ch;
}

/* A String or Bytes over a C heap copy of len bytes, GEAS_ERR_OOM when it cannot be had */
int64_t gd_set_buffer(int64_t v, int64_t ty, const void* bytes, int64_t len) {
    GeasValue* value = value_of(v);
    uint8_t* copy = malloc(len > 0 ? (size_t)len : 1);
    if (copy == NULL) {
        return GEAS_ERR_OOM;
    }
    if (len > 0) {
        memcpy(copy, bytes, (size_t)len);
    }
    set_scalar(value, (uint32_t)ty);
    value->as.s.ptr = copy;
    value->as.s.len = (uint64_t)len;
    return GEAS_OK;
}

/* Option or Result with a boxed payload, answers the payload slot or 0 when out of memory */
int64_t gd_set_box(int64_t v, int64_t ty, int64_t tag) {
    GeasValue* value = value_of(v);
    GeasValue* inner = calloc(1, sizeof(GeasValue));
    if (inner == NULL) {
        return 0;
    }
    set_scalar(value, (uint32_t)ty);
    value->tag = (uint32_t)tag;
    value->as.box = inner;
    return handle_of(inner);
}

/* The None of an Option, no box */
void gd_set_none(int64_t v) {
    set_scalar(value_of(v), GEAS_TY_OPTION);
}

/* A list shaped value with n zeroed elements, GEAS_ERR_OOM when they cannot be had */
int64_t gd_set_seq(int64_t v, int64_t ty, int64_t tag, int64_t elem_ty, int64_t n) {
    GeasValue* value = value_of(v);
    GeasValue* data = NULL;
    if (n > 0) {
        data = calloc((size_t)n, sizeof(GeasValue));
        if (data == NULL) {
            return GEAS_ERR_OOM;
        }
    }
    set_scalar(value, (uint32_t)ty);
    value->tag = (uint32_t)tag;
    value->as.list.data = data;
    value->as.list.len = (uint64_t)n;
    value->as.list.cap = (uint64_t)n;
    value->as.list.elem_ty = (uint32_t)elem_ty;
    return GEAS_OK;
}

/* ---- vow overrides ---- */

int64_t gd_vows_new(int64_t n) {
    size_t count = n > 0 ? (size_t)n : 1;
    return handle_of(calloc(count, sizeof(GeasVowBinding)));
}

/* Names the i-th override and answers its value slot, 0 when out of memory */
int64_t gd_vow_slot(int64_t vows, int64_t i, const char* name) {
    GeasVowBinding* row = &((GeasVowBinding*)(intptr_t)vows)[i];
    row->name = strdup(name);
    if (row->name == NULL) {
        return 0;
    }
    return handle_of(&row->value);
}

void gd_vows_free(int64_t vows, int64_t n) {
    GeasVowBinding* rows = (GeasVowBinding*)(intptr_t)vows;
    if (rows == NULL) {
        return;
    }
    for (int64_t i = 0; i < n; i++) {
        free((char*)rows[i].name);
        tree_free(&rows[i].value);
    }
    free(rows);
}

/* ---- the trampolines ---- */

typedef int64_t (*GdDispatchFn)(void* slot, int64_t size, int64_t ctx, int64_t args,
                                int64_t nargs, int64_t out);
typedef int64_t (*GdNowFn)(void* slot, int64_t size);
typedef void (*GdSleepFn)(void* slot, int64_t size, int64_t ms);

/* One thing the runtime calls back into dusk through, a binding or a clock */
typedef struct GdHook {
    struct GdHook* next;
    GeasRuntime* rt;
    GdDispatchFn dispatch;
    GdNowFn now;
    GdSleepFn sleep;
    int64_t size;
    unsigned char slot[];
} GdHook;

static GdHook* hooks = NULL;
static pthread_mutex_t hooks_lock = PTHREAD_MUTEX_INITIALIZER;

/* Copies the dusk slot onto a hook the runtime can hold, 0 when out of memory */
static GdHook* hook_new(GeasRuntime* rt, const void* slot, int64_t size) {
    if (size < 0) {
        return NULL;
    }
    GdHook* hook = calloc(1, sizeof(GdHook) + (size_t)size);
    if (hook == NULL) {
        return NULL;
    }
    hook->rt = rt;
    hook->size = size;
    memcpy(hook->slot, slot, (size_t)size);
    pthread_mutex_lock(&hooks_lock);
    hook->next = hooks;
    hooks = hook;
    pthread_mutex_unlock(&hooks_lock);
    return hook;
}

static GeasStatus gd_pledge_tramp(void* user, void* ctx, const GeasValue* args,
                                  size_t nargs, GeasValue* out) {
    GdHook* hook = user;
    int64_t status = hook->dispatch(hook->slot, hook->size, handle_of(ctx), handle_of(args),
                                    (int64_t)nargs, handle_of(out));
    return (GeasStatus)status;
}

static int64_t gd_now(void* user) {
    GdHook* hook = user;
    return hook->now(hook->slot, hook->size);
}

static void gd_sleep(void* user, uint64_t ms) {
    GdHook* hook = user;
    hook->sleep(hook->slot, hook->size, (int64_t)ms);
}

/* Binds a pledge or remedy to a dusk dispatch over a copy of slot */
int64_t gd_bind(int64_t rt, const char* name, const void* slot, int64_t size, GdDispatchFn fn) {
    GeasRuntime* runtime = (GeasRuntime*)(intptr_t)rt;
    GdHook* hook = hook_new(runtime, slot, size);
    if (hook == NULL) {
        return GEAS_ERR_OOM;
    }
    hook->dispatch = fn;
    return geas_pledge_bind_user(runtime, name, gd_pledge_tramp, hook);
}

/* Points the runtime at a dusk clock over a copy of slot */
int64_t gd_set_clock(int64_t rt, const void* slot, int64_t size, GdNowFn now, GdSleepFn sleep) {
    GeasRuntime* runtime = (GeasRuntime*)(intptr_t)rt;
    GdHook* hook = hook_new(runtime, slot, size);
    if (hook == NULL) {
        return GEAS_ERR_OOM;
    }
    hook->now = now;
    hook->sleep = sleep;
    return geas_runtime_set_clock(runtime, gd_now, gd_sleep, hook);
}

/* Puts the runtime back on CLOCK_REALTIME and its own sleep */
int64_t gd_default_clock(int64_t rt) {
    return geas_runtime_set_clock((GeasRuntime*)(intptr_t)rt, NULL, NULL, NULL);
}

/* Frees every hook of a runtime, called once the runtime is shut down */
void gd_runtime_release(int64_t rt) {
    GeasRuntime* runtime = (GeasRuntime*)(intptr_t)rt;
    pthread_mutex_lock(&hooks_lock);
    GdHook** link = &hooks;
    while (*link != NULL) {
        GdHook* hook = *link;
        if (hook->rt != runtime) {
            link = &hook->next;
            continue;
        }
        *link = hook->next;
        free(hook);
    }
    pthread_mutex_unlock(&hooks_lock);
}

/* ---- calls that take a NULL ---- */

/* An instance setting, an empty item selecting the contract itself */
int64_t gd_instance_attr(int64_t c, const char* item, const char* key, int64_t* out) {
    const GeasValue* value = NULL;
    const char* named = item[0] == '\0' ? NULL : item;
    GeasStatus status = geas_instance_attr((const GeasContract*)(intptr_t)c, named, key, &value);
    out[0] = handle_of(value);
    return status;
}

/* A fulfillment with no by reference arguments, sync */
int64_t gd_fulfill_sync(int64_t c, const char* pledge, int64_t args, int64_t nargs, int64_t out) {
    return geas_pledge_fulfill_sync((GeasContract*)(intptr_t)c, pledge, value_of(args),
                                    (size_t)nargs, NULL, 0, value_of(out));
}

/* A fulfillment with no by reference arguments, answering its future or 0 */
int64_t gd_fulfill(int64_t c, const char* pledge, int64_t args, int64_t nargs) {
    return handle_of(geas_pledge_fulfill((GeasContract*)(intptr_t)c, pledge, value_of(args),
                                         (size_t)nargs, NULL, 0));
}
