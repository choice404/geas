/* Host settings before sign, on an instance, and across descriptor sizes */
#include <geas/geas.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    uint32_t owner;
    const char* item;
    const char* key;
    uint32_t type;
    int64_t integer;
    const char* string;
} expected[] = {
    {GEAS_ATTR_CONTRACT, "Job", "queue", GEAS_TY_STRING, 0, "batch"},
    {GEAS_ATTR_CONTRACT, "Job", "priority", GEAS_TY_INT, -2, NULL},
    {GEAS_ATTR_CONTRACT, "Job", "weight", GEAS_TY_FLOAT, 0, NULL},
    {GEAS_ATTR_CONTRACT, "Job", "enabled", GEAS_TY_BOOL, 1, NULL},
    {GEAS_ATTR_CONTRACT, "Job", "delay", GEAS_TY_INT, -250, NULL},
    {GEAS_ATTR_PLEDGE, "copy", "program", GEAS_TY_STRING, 0, "/bin/copy"},
    {GEAS_ATTR_PLEDGE, "copy", "region", GEAS_TY_INT, 67108864, NULL},
    {GEAS_ATTR_PLEDGE, "copy", "region_max", GEAS_TY_INT, 536870912, NULL},
    {GEAS_ATTR_PLEDGE, "copy", "threads", GEAS_TY_INT, 1, NULL},
    {GEAS_ATTR_SUBCONTRACT, "Finish", "pool", GEAS_TY_STRING, 0, "local"},
    {GEAS_ATTR_SIGNAL, "ready", "channel", GEAS_TY_STRING, 0, "job.ready"},
    {GEAS_ATTR_REMEDY, "undo", "program", GEAS_TY_STRING, 0, "/bin/remove"},
};

static void check_value(size_t index, const GeasValue* value) {
    assert(value && value->ty == expected[index].type);
    switch (value->ty) {
    case GEAS_TY_INT:
        assert(value->as.i == expected[index].integer);
        break;
    case GEAS_TY_FLOAT:
        assert(value->as.f == -1.5);
        break;
    case GEAS_TY_BOOL:
        assert(value->as.b == 1);
        break;
    case GEAS_TY_STRING:
        assert(value->as.s.len == strlen(expected[index].string));
        assert(memcmp(value->as.s.ptr, expected[index].string, value->as.s.len) == 0);
        break;
    default:
        assert(0);
    }
}

static void check_all(GeasRuntime* rt, GeasContract* contract) {
    size_t count = 0;
    GeasStatus status = contract ? geas_instance_attr_count(contract, &count)
                                : geas_attr_count(rt, "Job", &count);
    assert(status == GEAS_OK && count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < count; i++) {
        GeasAttrInfo info;
        status = contract ? geas_instance_attr_at(contract, i, &info)
                          : geas_attr_at(rt, "Job", i, &info);
        assert(status == GEAS_OK);
        assert(info.owner == expected[i].owner);
        assert(strcmp(info.owner_name, expected[i].item) == 0);
        assert(strcmp(info.key, expected[i].key) == 0);
        check_value(i, info.value);
        const char* item = info.owner == GEAS_ATTR_CONTRACT ? NULL : info.owner_name;
        const GeasValue* value = NULL;
        if (contract) status = geas_instance_attr(contract, item, info.key, &value);
        else if (item) status = geas_item_attr(rt, "Job", item, info.key, &value);
        else status = geas_contract_attr(rt, "Job", info.key, &value);
        assert(status == GEAS_OK && value == info.value);
    }
}

static void check_missing(GeasRuntime* rt, GeasContract* contract) {
    const GeasValue* value = NULL;
    GeasAttrInfo info;
    size_t count;
    assert(geas_attr_count(rt, "missing", &count) == GEAS_ERR_NAME);
    assert(geas_attr_at(rt, "missing", 0, &info) == GEAS_ERR_NAME);
    assert(geas_attr_at(rt, "Job", 99, &info) == GEAS_ERR_NAME);
    assert(geas_contract_attr(rt, "missing", "queue", &value) == GEAS_ERR_NAME);
    assert(geas_item_attr(rt, "missing", "copy", "program", &value) == GEAS_ERR_NAME);
    assert(geas_item_attr(rt, "Job", "missing", "program", &value) == GEAS_ERR_NAME);
    assert(geas_contract_attr(rt, "Job", "queue", &value) == GEAS_OK && value);
    assert(geas_contract_attr(rt, "Job", "absent", &value) == GEAS_OK && !value);
    assert(geas_item_attr(rt, "Job", "flush", "program", &value) == GEAS_OK && !value);
    assert(geas_item_attr(rt, "Job", "copy", "absent", &value) == GEAS_OK && !value);
    assert(geas_instance_attr(contract, "missing", "program", &value) == GEAS_ERR_NAME);
    assert(geas_instance_attr(contract, NULL, "absent", &value) == GEAS_OK && !value);
    assert(geas_instance_attr(contract, "copy", "absent", &value) == GEAS_OK && !value);
    assert(geas_attr_count(NULL, "Job", &count) == GEAS_ERR_TYPE);
    assert(geas_contract_attr(rt, "Job", "queue", NULL) == GEAS_ERR_TYPE);
}

static void check_short_descriptors(GeasRuntime* rt) {
    const GeasContractDesc full = {.name = "Older", .version = 1};
    void* older = malloc(GEAS_CONTRACT_DESC_V2_SIZE);
    assert(older);
    memcpy(older, &full, GEAS_CONTRACT_DESC_V2_SIZE);
    assert(geas_register_contract_sized(rt, older, GEAS_CONTRACT_DESC_V2_SIZE) == GEAS_OK);
    free(older);
    size_t count = 99;
    assert(geas_attr_count(rt, "Older", &count) == GEAS_OK && count == 0);
    GeasContract* contract;
    assert(geas_contract_sign(rt, "Older", NULL, 0, 0, &contract) == GEAS_OK);
    assert(geas_instance_attr_count(contract, &count) == GEAS_OK && count == 0);
    assert(geas_contract_break(contract) == GEAS_OK);
}

static void check_tail_validation(GeasRuntime* rt) {
    GeasAttrDesc attr = {.owner = GEAS_ATTR_PLEDGE, .name = "setting",
                         .value = {.ty = GEAS_TY_INT, .as.i = 1}};
    GeasContractDesc desc = {.name = "Invalid", .nattrs = 1, .attrs = &attr};
    assert(geas_register_contract(rt, &desc) == GEAS_ERR_TYPE);
    attr.owner = GEAS_ATTR_CONTRACT;
    attr.value.ty = GEAS_TY_LIST;
    assert(geas_register_contract(rt, &desc) == GEAS_ERR_TYPE);
    desc.attrs = NULL;
    assert(geas_register_contract(rt, &desc) == GEAS_ERR_TYPE);
    desc.name = "PartialTail";
    size_t prefix_size = offsetof(GeasContractDesc, attrs);
    void* prefix = malloc(prefix_size);
    assert(prefix);
    memcpy(prefix, &desc, prefix_size);
    assert(geas_register_contract_sized(rt, prefix, prefix_size) == GEAS_OK);
    free(prefix);
    size_t count = 99;
    assert(geas_attr_count(rt, "PartialTail", &count) == GEAS_OK && count == 0);
}

static void check_anonymous(GeasRuntime* rt) {
    static const char* const subs[] = {NULL};
    static const GeasAttrDesc attrs[] = {
        {.owner = GEAS_ATTR_SUBCONTRACT, .name = "pool",
         .value = {.ty = GEAS_TY_INT, .as.i = 7}},
    };
    static const GeasContractDesc desc = {
        .name = "Anonymous", .nsubs = 1, .subs = subs, .nattrs = 1, .attrs = attrs,
    };
    assert(geas_register_contract(rt, &desc) == GEAS_OK);
    GeasAttrInfo info;
    assert(geas_attr_at(rt, "Anonymous", 0, &info) == GEAS_OK);
    assert(info.owner == GEAS_ATTR_SUBCONTRACT && !info.owner_name);
    assert(info.value->ty == GEAS_TY_INT && info.value->as.i == 7);
}

static void check_changed_build(uint64_t shape_hash) {
    GeasRuntime* rt;
    assert(geas_runtime_init(NULL, &rt) == GEAS_OK);
    assert(geas_module_load(rt, "target/geas-out/libjobstep_changed.geas.so") == GEAS_OK);
    GeasContract* contract;
    assert(geas_contract_sign(rt, "Job", NULL, 0, shape_hash, &contract) == GEAS_OK);
    assert(geas_contract_hash(contract) == shape_hash);
    const GeasValue* value;
    assert(geas_contract_attr(rt, "Job", "priority", &value) == GEAS_OK);
    assert(value && value->as.i == -3);
    assert(geas_contract_break(contract) == GEAS_OK);
    geas_runtime_shutdown(rt);
}

int main(void) {
    GeasRuntime* rt;
    assert(geas_runtime_init(NULL, &rt) == GEAS_OK);
    assert(geas_module_load(rt, "target/geas-out/libjobstep.geas.so") == GEAS_OK);
    check_tail_validation(rt);
    check_anonymous(rt);
    check_short_descriptors(rt);
    check_all(rt, NULL);
    GeasContract* contract;
    assert(geas_contract_sign(rt, "Job", NULL, 0, 0, &contract) == GEAS_OK);
    check_all(rt, contract);
    check_missing(rt, contract);
    uint64_t shape_hash = geas_contract_hash(contract);
    assert(geas_contract_break(contract) == GEAS_OK);
    check_all(rt, contract);
    geas_runtime_shutdown(rt);
    check_changed_build(shape_hash);
    puts("[attrs] passed");
    return 0;
}
