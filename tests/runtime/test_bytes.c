/* Raw records through compiled pledges, the host, and a parked instance */
#define TEST_NAME "test_bytes"
#include "workflow_util.h"
#include <geas/geas_wire.h>
#include <unistd.h>

static const uint8_t raw_record[] = { 0, 0xff, 0x80, 'A', 0 };
static atomic_int undo_calls;

static GeasValue borrowed(const uint8_t* ptr, uint64_t len) {
    GeasValue value = { .ty = GEAS_TY_BYTES };
    value.as.s.ptr = (uint8_t*)ptr;
    value.as.s.len = len;
    return value;
}

static int bytes_are(const GeasValue* value, const uint8_t* ptr, uint64_t len) {
    return value && value->ty == GEAS_TY_BYTES && value->as.s.len == len &&
        (!len || memcmp(value->as.s.ptr, ptr, len) == 0);
}

static const GeasValue* payload(const GeasValue* value) {
    CHECK(value->ty == GEAS_TY_RESULT && value->tag == 0 && value->as.box,
          "pledge returns Ok");
    return value->as.box;
}

static GeasStatus host_transform(void* ctx, const GeasValue* args, size_t count,
                                  GeasValue* out) {
    if (count != 1 || args[0].ty != GEAS_TY_BYTES) return GEAS_ERR_TYPE;
    GeasValue suffix = borrowed(raw_record, sizeof(raw_record));
    GeasValue joined;
    GeasStatus status = geas_bytes_concat(ctx, &args[0], &suffix, &joined);
    if (status != GEAS_OK) return status;
    /* The frame slot is mutable, including when it came from a GeasRef */
    ((GeasValue*)args)[0] = joined;
    return result_of(ctx, 0, &joined, out);
}

static GeasStatus host_undo(void* ctx, const GeasValue* args, size_t count,
                             GeasValue* out) {
    (void)ctx;
    CHECK(count == 1 && bytes_are(args, raw_record, sizeof(raw_record)),
          "remedy receives the parked Bytes Ok payload");
    atomic_fetch_add(&undo_calls, 1);
    memset(out, 0, sizeof(*out));
    return GEAS_OK;
}

static GeasRuntime* blob_runtime(void) {
    GeasRuntime* runtime = fake_runtime();
    if (!runtime) return NULL;
    int is_ready = geas_module_load(runtime, "target/geas-out/libblob.geas.so") == GEAS_OK &&
        geas_pledge_bind(runtime, "Blob.host", host_transform) == GEAS_OK &&
        geas_pledge_bind(runtime, "Blob.undo", host_undo) == GEAS_OK;
    CHECK(is_ready, "load the compiled module and bind the host pledges");
    if (!is_ready) {
        geas_runtime_shutdown(runtime);
        return NULL;
    }
    return runtime;
}

static GeasContract* sign_blob(GeasRuntime* runtime) {
    GeasContract* instance = NULL;
    GeasVowBinding vow = { "seed", borrowed(raw_record, sizeof(raw_record)) };
    CHECK(geas_contract_sign(runtime, "Blob", NULL, 0, 0, &instance) == GEAS_ERR_UNBOUND,
          "Bytes vow is required at sign");
    CHECK(geas_contract_sign(runtime, "Blob", &vow, 1, 0, &instance) == GEAS_OK,
          "sign with a Bytes vow");
    return instance;
}

static void test_values(GeasContract* instance) {
    GeasValue raw = borrowed(raw_record, sizeof(raw_record));
    GeasValue out;
    CHECK(run(instance, "raw", &raw, 1, &out) == GEAS_OK &&
          bytes_are(&out, raw_record, sizeof(raw_record)), "bare Bytes return");
    CHECK(run(instance, "echo", &raw, 1, &out) == GEAS_OK, "compiled echo");
    CHECK(bytes_are(payload(&out), raw_record, sizeof(raw_record)), "embedded zeros and invalid UTF-8");
    CHECK(((const GeasValue*)out.as.box)->as.s.ptr != raw.as.s.ptr, "argument is deeply copied");
    CHECK(run(instance, "count", &raw, 1, &out) == GEAS_OK, "compiled len");
    CHECK(payload(&out)->as.i == (int64_t)sizeof(raw_record), "len counts bytes");
    GeasValue pair[] = { raw, raw };
    CHECK(run(instance, "equal", pair, 2, &out) == GEAS_OK && payload(&out)->as.b, "equality");
    pair[1].as.s.len--;
    CHECK(run(instance, "equal", pair, 2, &out) == GEAS_OK && !payload(&out)->as.b, "unequal lengths");
    const uint8_t other[] = { 0, 0xfe, 0x80, 'A', 0 };
    pair[1] = borrowed(other, sizeof(other));
    CHECK(run(instance, "equal", pair, 2, &out) == GEAS_OK && !payload(&out)->as.b, "unequal bytes");
    pair[1] = raw;
    CHECK(run(instance, "join", pair, 2, &out) == GEAS_OK, "concatenation");
    const GeasValue* joined = payload(&out);
    CHECK(joined->as.s.len == 2 * sizeof(raw_record) &&
          memcmp(joined->as.s.ptr, raw_record, sizeof(raw_record)) == 0 &&
          memcmp(joined->as.s.ptr + sizeof(raw_record), raw_record, sizeof(raw_record)) == 0,
          "concatenation preserves every byte");
    pair[1] = int_val(1);
    CHECK(run(instance, "at", pair, 2, &out) == GEAS_OK && payload(&out)->as.b == 255, "Byte index");
    pair[1] = int_val(-1);
    CHECK(run(instance, "at", pair, 2, &out) == GEAS_ERR_TYPE, "negative index faults");
    pair[1] = int_val(sizeof(raw_record));
    CHECK(run(instance, "at", pair, 2, &out) == GEAS_ERR_TYPE, "index at length faults");
    CHECK(run(instance, "host", &raw, 1, &out) == GEAS_OK &&
          payload(&out)->as.s.len == 2 * sizeof(raw_record), "host receives and returns Bytes");
}

static void test_conversions(GeasContract* instance) {
    const uint8_t utf8[] = { 'a', 0, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80 };
    GeasValue string = borrowed(utf8, sizeof(utf8));
    string.ty = GEAS_TY_STRING;
    GeasValue out;
    CHECK(run(instance, "encode", &string, 1, &out) == GEAS_OK &&
          bytes_are(payload(&out), utf8, sizeof(utf8)), "String to UTF-8 Bytes");
    GeasValue raw = *payload(&out);
    CHECK(run(instance, "decode", &raw, 1, &out) == GEAS_OK && out.tag == 1, "valid UTF-8 is Some");
    const GeasValue* text = out.as.box;
    CHECK(text->ty == GEAS_TY_STRING && text->as.s.len == sizeof(utf8) &&
          memcmp(text->as.s.ptr, utf8, sizeof(utf8)) == 0, "text keeps zeros and Unicode");
    const uint8_t invalid[][4] = {
        { 0x80 }, { 0xc0, 0x80 }, { 0xe0, 0x80, 0x80 }, { 0xed, 0xa0, 0x80 },
        { 0xf4, 0x90, 0x80, 0x80 }, { 0xf5, 0x80, 0x80, 0x80 }, { 0xe2, 'a' }, { 0xe2, 0x82 }
    };
    const size_t lengths[] = { 1, 2, 3, 3, 4, 4, 2, 2 };
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        raw = borrowed(invalid[i], lengths[i]);
        CHECK(run(instance, "decode", &raw, 1, &out) == GEAS_OK &&
              out.ty == GEAS_TY_OPTION && out.tag == 0, "invalid UTF-8 is None");
    }
    raw = borrowed(raw_record, sizeof(raw_record));
    CHECK(run(instance, "list", &raw, 1, &out) == GEAS_OK, "Bytes to List<Byte>");
    GeasValue list = *payload(&out);
    CHECK(list.ty == GEAS_TY_LIST && list.as.list.elem_ty == GEAS_TY_BYTE &&
          list.as.list.len == sizeof(raw_record), "Byte list shape");
    CHECK(run(instance, "pack", &list, 1, &out) == GEAS_OK &&
          bytes_are(payload(&out), raw_record, sizeof(raw_record)), "List<Byte> back to Bytes");
    raw = borrowed(NULL, 0);
    CHECK(run(instance, "decode", &raw, 1, &out) == GEAS_OK && out.tag == 1 &&
          ((GeasValue*)out.as.box)->as.s.len == 0, "empty Bytes are valid text");
    CHECK(run(instance, "list", &raw, 1, &out) == GEAS_OK, "empty Byte list");
    list = *payload(&out);
    CHECK(run(instance, "pack", &list, 1, &out) == GEAS_OK &&
          bytes_are(payload(&out), NULL, 0), "empty Byte list packs");
}

typedef struct BytesWriteBack {
    uint8_t bytes[2 * sizeof(raw_record)];
    uint64_t len;
} BytesWriteBack;

static void copy_back(void* host, const GeasValue* value, void* user) {
    (void)host;
    BytesWriteBack* copied = user;
    CHECK(value->ty == GEAS_TY_BYTES && value->as.s.len <= sizeof(copied->bytes),
          "custom write back gets Bytes within capacity");
    if (value->as.s.len > sizeof(copied->bytes)) return;
    memcpy(copied->bytes, value->as.s.ptr, value->as.s.len);
    copied->len = value->as.s.len;
}

static void test_helpers(GeasContract* instance) {
    GeasValue raw = geas_bytes_copy(instance, raw_record, sizeof(raw_record));
    GeasValue copy;
    CHECK(geas_bytes_copy_into(instance, raw_record, sizeof(raw_record), &copy) == GEAS_OK &&
          geas_value_eq(&raw, &copy), "both allocation helpers agree");
    CHECK(geas_value_deep_copy(instance, &raw, &copy) == GEAS_OK &&
          copy.as.s.ptr != raw.as.s.ptr && geas_value_eq(&raw, &copy), "deep copy owns a new buffer");
    GeasValue string = raw;
    string.ty = GEAS_TY_STRING;
    CHECK(!geas_value_eq(&raw, &string), "Bytes and String are distinct");
    CHECK(geas_bytes_copy_into(instance, NULL, 1, &copy) == GEAS_ERR_TYPE, "NULL bytes refused");
    CHECK(geas_bytes_copy_into(instance, raw_record, UINT64_MAX, &copy) == GEAS_ERR_OOM,
          "oversized allocation refused");
    GeasString host = { (uint8_t*)raw_record, sizeof(raw_record) };
    GeasRef ref = { .host_ptr = &host, .ty = GEAS_TY_BYTES };
    GeasValue out;
    CHECK(geas_pledge_fulfill_sync(instance, "host", NULL, 0, &ref, 1, &out) == GEAS_OK,
          "Bytes by reference");
    CHECK(host.len == 2 * sizeof(raw_record) && host.ptr != raw_record,
          "default write back points to instance owned bytes");
    BytesWriteBack copied = { { 0 }, 0 };
    host.ptr = (uint8_t*)raw_record;
    host.len = sizeof(raw_record);
    ref.write_back = copy_back;
    ref.user = &copied;
    GeasFuture* future = geas_pledge_fulfill(instance, "host", NULL, 0, &ref, 1);
    CHECK(future != NULL, "async reference call");
    if (future) {
        CHECK(geas_future_wait(future, &out) == GEAS_OK, "custom write back at wait");
        CHECK(geas_future_release(future) == GEAS_OK, "release reference receipt");
    }
    CHECK(copied.len == 2 * sizeof(raw_record) &&
          memcmp(copied.bytes, raw_record, sizeof(raw_record)) == 0 &&
          memcmp(copied.bytes + sizeof(raw_record), raw_record, sizeof(raw_record)) == 0,
          "custom write back owns the transformed bytes");
    CHECK(geas_bytes_text(instance, &string, &out) == GEAS_ERR_TYPE, "text helper requires Bytes");
}

static void test_large(GeasContract* instance) {
    const size_t size = 1024 * 1024;
    uint8_t* data = malloc(size);
    CHECK(data != NULL, "allocate 1 MiB host buffer");
    if (!data) return;
    for (size_t i = 0; i < size; i++) data[i] = (uint8_t)i;
    GeasValue raw = borrowed(data, size);
    GeasValue out;
    CHECK(run(instance, "echo", &raw, 1, &out) == GEAS_OK &&
          bytes_are(payload(&out), data, size), "1 MiB compiled round trip");
    CHECK(run(instance, "count", &raw, 1, &out) == GEAS_OK &&
          payload(&out)->as.i == (int64_t)size, "1 MiB byte count");
    char rendered[160];
    size_t need = 0;
    CHECK(geas_value_render(&raw, rendered, sizeof(rendered), &need) == GEAS_OK &&
          strstr(rendered, "00010203") && strstr(rendered, "...; 1048576 bytes)"),
          "debug hex is bounded for a large buffer");
    size_t encoded_len = 0;
    CHECK(geas_wire_encode_value(&raw, NULL, 0, &encoded_len) == GEAS_ERR_OOM,
          "size the 1 MiB wire value");
    uint8_t* encoded = malloc(encoded_len);
    CHECK(encoded != NULL, "allocate wire buffer");
    if (encoded) {
        CHECK(geas_wire_encode_value(&raw, encoded, encoded_len, &encoded_len) == GEAS_OK,
              "encode 1 MiB");
        size_t consumed = 0;
        CHECK(geas_wire_decode_value(instance, encoded, encoded_len, &out, &consumed) == GEAS_OK &&
              consumed == encoded_len && bytes_are(&out, data, size), "decode 1 MiB");
        free(encoded);
    }
    free(data);
}

static void test_park(void) {
    char path[] = "/tmp/geas-bytes-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "create park store");
    if (fd < 0) return;
    close(fd);
    GeasRuntime* runtime = blob_runtime();
    if (!runtime) { unlink(path); return; }
    GeasContract* instance = sign_blob(runtime);
    GeasValue raw = borrowed(raw_record, sizeof(raw_record));
    GeasValue out;
    CHECK(run(instance, "echo", &raw, 1, &out) == GEAS_OK, "Ok before park");
    CHECK(run(instance, "failure", &raw, 1, &out) == GEAS_OK && out.tag == 1, "Err before park");
    CHECK(geas_signal(instance, "received", &raw, 1) == GEAS_OK, "Bytes signal");
    CHECK(geas_instance_park(instance, path, "blob") == GEAS_OK, "park Bytes values");
    geas_runtime_shutdown(runtime);
    atomic_store(&undo_calls, 0);
    runtime = blob_runtime();
    if (!runtime) { unlink(path); return; }
    instance = NULL;
    CHECK(geas_instance_resume(runtime, path, "blob", 0, &instance) == GEAS_OK, "resume Bytes values");
    if (instance) {
        CHECK(bytes_are(geas_vow_ref(instance, "seed"), raw_record, sizeof(raw_record)), "vow survived");
        const GeasValue* signal = NULL;
        CHECK(geas_signal_payload(instance, "received", &signal) == GEAS_OK &&
              bytes_are(signal, raw_record, sizeof(raw_record)), "signal survived");
        const char* name = NULL;
        const GeasValue* error = NULL;
        CHECK(geas_partial_error(instance, 0, &name, &error) == GEAS_OK &&
              !strcmp(name, "failure") && bytes_are(error, raw_record, sizeof(raw_record)), "Err survived");
        CHECK(geas_contract_break(instance) == GEAS_OK, "break the resumed instance");
        CHECK(atomic_load(&undo_calls) == 1, "remedy ran once with the resumed Ok");
    }
    geas_runtime_shutdown(runtime);
    unlink(path);
}

int main(void) {
    GeasRuntime* runtime = blob_runtime();
    if (!runtime) return 1;
    GeasContract* instance = sign_blob(runtime);
    if (instance) {
        test_values(instance);
        test_conversions(instance);
        test_helpers(instance);
        test_large(instance);
    }
    geas_runtime_shutdown(runtime);
    test_park();
    if (atomic_load(&g_failures)) return 1;
    puts("[test_bytes] ok");
    return 0;
}
