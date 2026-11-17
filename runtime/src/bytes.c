/* Raw byte values and their explicit String and List<Byte> conversions */
#include "runtime_internal.h"

#include <string.h>

GeasValue geas_bytes_copy(GeasContract* c, const uint8_t* ptr, uint64_t len) {
    GeasValue value;
    memset(&value, 0, sizeof(value));
    value.ty = GEAS_TY_BYTES;
    if (!c || (!ptr && len) || len > SIZE_MAX - sizeof(GeasBlock)) return value;
    value = geas_string_copy(c, ptr, len);
    value.ty = GEAS_TY_BYTES;
    return value;
}

GeasStatus geas_bytes_copy_into(GeasContract* c, const uint8_t* ptr,
                                uint64_t len, GeasValue* out) {
    if (!c || !out || (!ptr && len)) return GEAS_ERR_TYPE;
    if (len > SIZE_MAX - sizeof(GeasBlock)) return GEAS_ERR_OOM;
    *out = geas_bytes_copy(c, ptr, len);
    return out->as.s.ptr ? GEAS_OK : GEAS_ERR_OOM;
}

static int valid_bytes(const GeasValue* value) {
    return value && value->ty == GEAS_TY_BYTES &&
        (value->as.s.len == 0 || value->as.s.ptr);
}

GeasStatus geas_bytes_concat(GeasContract* c, const GeasValue* a,
                             const GeasValue* b, GeasValue* out) {
    if (!c || !out || !valid_bytes(a) || !valid_bytes(b)) return GEAS_ERR_TYPE;
    const uint64_t max_len = SIZE_MAX - sizeof(GeasBlock);
    if (a->as.s.len > max_len || b->as.s.len > max_len - a->as.s.len) {
        return GEAS_ERR_OOM;
    }
    GeasValue left = *a;
    GeasValue right = *b;
    left.ty = right.ty = GEAS_TY_STRING;
    GeasStatus status = geas_string_concat(c, &left, &right, out);
    if (status == GEAS_OK) out->ty = GEAS_TY_BYTES;
    return status;
}

GeasStatus geas_bytes_from(GeasContract* c, const GeasValue* value, GeasValue* out) {
    if (!c || !value || !out) return GEAS_ERR_TYPE;
    if (value->ty == GEAS_TY_STRING) {
        return geas_bytes_copy_into(c, value->as.s.ptr, value->as.s.len, out);
    }
    if (value->ty != GEAS_TY_LIST || value->as.list.elem_ty != GEAS_TY_BYTE) {
        return GEAS_ERR_TYPE;
    }
    uint64_t count = value->as.list.len;
    const GeasValue* elements = value->as.list.data;
    if (count && !elements) return GEAS_ERR_TYPE;
    if (count > SIZE_MAX - sizeof(GeasBlock)) return GEAS_ERR_OOM;
    for (uint64_t i = 0; i < count; i++) {
        if (elements[i].ty != GEAS_TY_BYTE) return GEAS_ERR_TYPE;
    }
    uint8_t* ptr = geas_bytes(c, count);
    if (!ptr) return GEAS_ERR_OOM;
    for (uint64_t i = 0; i < count; i++) ptr[i] = elements[i].as.b;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_BYTES;
    out->as.s.ptr = ptr;
    out->as.s.len = count;
    return GEAS_OK;
}

/* Decode Unicode scalars, rejecting overlong encodings, surrogates, and
 * values above U+10FFFF. An embedded zero is ordinary ASCII */
static int valid_utf8(const uint8_t* ptr, uint64_t len) {
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
        if (scalar < minimum || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff)) return 0;
    }
    return 1;
}

GeasStatus geas_bytes_text(GeasContract* c, const GeasValue* value, GeasValue* out) {
    if (!c || !out || !valid_bytes(value)) return GEAS_ERR_TYPE;
    GeasString bytes = value->as.s;
    memset(out, 0, sizeof(*out));
    out->ty = GEAS_TY_OPTION;
    if (!valid_utf8(bytes.ptr, bytes.len)) return GEAS_OK;
    GeasValue* box = geas_box(c);
    if (!box) return GEAS_ERR_OOM;
    GeasStatus status = geas_string_copy_into(c, bytes.ptr, bytes.len, box);
    if (status != GEAS_OK) return status;
    out->tag = 1;
    out->as.box = box;
    return GEAS_OK;
}

GeasStatus geas_bytes_list(GeasContract* c, const GeasValue* value, GeasValue* out) {
    if (!c || !out || !valid_bytes(value)) return GEAS_ERR_TYPE;
    GeasString bytes = value->as.s;
    if (bytes.len > (SIZE_MAX - sizeof(GeasBlock)) / sizeof(GeasValue)) {
        return GEAS_ERR_OOM;
    }
    GeasStatus status = geas_list_new(c, GEAS_TY_BYTE, bytes.len, out);
    if (status != GEAS_OK) return status;
    GeasValue* elements = out->as.list.data;
    for (uint64_t i = 0; i < bytes.len; i++) {
        memset(&elements[i], 0, sizeof(elements[i]));
        elements[i].ty = GEAS_TY_BYTE;
        elements[i].as.b = bytes.ptr[i];
    }
    out->as.list.len = bytes.len;
    return GEAS_OK;
}
