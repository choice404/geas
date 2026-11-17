/* Declared attributes borrow immutable module storage until runtime shutdown */
#include "runtime_internal.h"

#include <string.h>

static int attr_owner_valid(const GeasContractDesc* desc, const GeasAttrDesc* attr) {
    switch (attr->owner) {
    case GEAS_ATTR_CONTRACT:
        return attr->owner_index == 0;
    case GEAS_ATTR_SUBCONTRACT:
        return attr->owner_index < desc->nsubs && desc->subs;
    case GEAS_ATTR_PLEDGE:
    case GEAS_ATTR_SIGNAL:
        if (attr->owner_index >= desc->npledges || !desc->pledges) return 0;
        return geas_rt_is_signal(desc, attr->owner_index) ==
               (attr->owner == GEAS_ATTR_SIGNAL);
    case GEAS_ATTR_REMEDY:
        return attr->owner_index < desc->nremedies && desc->remedies;
    default:
        return 0;
    }
}

/**
 * @params desc - the complete descriptor copy
 * @returns {GeasStatus} - the registration status
 */
GeasStatus geas_rt_validate_attrs(const GeasContractDesc* desc) {
    if (desc->nattrs && !desc->attrs) return GEAS_ERR_TYPE;
    for (uint32_t i = 0; i < desc->nattrs; i++) {
        const GeasAttrDesc* attr = &desc->attrs[i];
        if (!attr->name || attr->value.tag || !attr_owner_valid(desc, attr)) return GEAS_ERR_TYPE;
        switch (attr->value.ty) {
        case GEAS_TY_INT:
        case GEAS_TY_FLOAT:
            break;
        case GEAS_TY_BOOL:
            if (attr->value.as.b > 1) return GEAS_ERR_TYPE;
            break;
        case GEAS_TY_STRING:
            if (attr->value.as.s.len && !attr->value.as.s.ptr) return GEAS_ERR_TYPE;
            break;
        default:
            return GEAS_ERR_TYPE;
        }
    }
    return GEAS_OK;
}

static const GeasContractDesc* attr_contract(GeasRuntime* rt, const char* name) {
    const GeasContractDesc* desc = NULL;
    pthread_mutex_lock(&rt->lock);
    for (size_t i = 0; i < rt->ndescs; i++) {
        if (strcmp(rt->descs[i]->name, name) != 0) continue;
        desc = rt->descs[i];
        break;
    }
    pthread_mutex_unlock(&rt->lock);
    return desc;
}

static const char* attr_owner_name(const GeasContractDesc* desc,
                                   const GeasAttrDesc* attr) {
    switch (attr->owner) {
    case GEAS_ATTR_CONTRACT: return desc->name;
    case GEAS_ATTR_SUBCONTRACT: return desc->subs[attr->owner_index];
    case GEAS_ATTR_PLEDGE:
    case GEAS_ATTR_SIGNAL: return desc->pledges[attr->owner_index].name;
    case GEAS_ATTR_REMEDY: return desc->remedies[attr->owner_index].name;
    default: return NULL;
    }
}

static GeasStatus attr_at(const GeasContractDesc* desc, size_t index,
                           GeasAttrInfo* out) {
    if (index >= desc->nattrs) return GEAS_ERR_NAME;
    const GeasAttrDesc* attr = &desc->attrs[index];
    out->owner = attr->owner;
    out->owner_name = attr_owner_name(desc, attr);
    out->key = attr->name;
    out->value = &attr->value;
    return GEAS_OK;
}

static GeasStatus attr_item(const GeasContractDesc* desc, const char* name,
                             uint32_t* owner, uint32_t* index) {
    if (!name) {
        *owner = GEAS_ATTR_CONTRACT;
        *index = 0;
        return GEAS_OK;
    }
    for (uint32_t i = 0; i < desc->npledges; i++) {
        if (strcmp(desc->pledges[i].name, name) != 0) continue;
        *owner = geas_rt_is_signal(desc, i) ? GEAS_ATTR_SIGNAL : GEAS_ATTR_PLEDGE;
        *index = i;
        return GEAS_OK;
    }
    for (uint32_t i = 0; i < desc->nsubs; i++) {
        if (!desc->subs[i] || strcmp(desc->subs[i], name) != 0) continue;
        *owner = GEAS_ATTR_SUBCONTRACT;
        *index = i;
        return GEAS_OK;
    }
    for (uint32_t i = 0; i < desc->nremedies; i++) {
        if (strcmp(desc->remedies[i].name, name) != 0) continue;
        *owner = GEAS_ATTR_REMEDY;
        *index = i;
        return GEAS_OK;
    }
    return GEAS_ERR_NAME;
}

static GeasStatus attr_find(const GeasContractDesc* desc, const char* item,
                             const char* key, const GeasValue** out) {
    uint32_t owner, index;
    GeasStatus status = attr_item(desc, item, &owner, &index);
    if (status != GEAS_OK) return status;
    for (uint32_t i = 0; i < desc->nattrs; i++) {
        const GeasAttrDesc* attr = &desc->attrs[i];
        if (attr->owner != owner || attr->owner_index != index) continue;
        if (strcmp(attr->name, key) != 0) continue;
        *out = &attr->value;
        return GEAS_OK;
    }
    return GEAS_OK;
}

/**
 * @params rt - the runtime, contract - its name, count - the output count
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_attr_count(GeasRuntime* rt, const char* contract, size_t* count) {
    if (!count) return GEAS_ERR_TYPE;
    *count = 0;
    if (!rt || !contract) return GEAS_ERR_TYPE;
    const GeasContractDesc* desc = attr_contract(rt, contract);
    if (!desc) return GEAS_ERR_NAME;
    *count = desc->nattrs;
    return GEAS_OK;
}

/**
 * @params rt - the runtime, contract - its name, index - the row, out - the borrowed row
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_attr_at(GeasRuntime* rt, const char* contract, size_t index,
                        GeasAttrInfo* out) {
    if (!out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (!rt || !contract) return GEAS_ERR_TYPE;
    const GeasContractDesc* desc = attr_contract(rt, contract);
    if (!desc) return GEAS_ERR_NAME;
    return attr_at(desc, index, out);
}

/**
 * @params rt - the runtime, contract - its name, key - the setting, out - the borrowed value
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_contract_attr(GeasRuntime* rt, const char* contract, const char* key,
                              const GeasValue** out) {
    if (!out) return GEAS_ERR_TYPE;
    *out = NULL;
    if (!rt || !contract || !key) return GEAS_ERR_TYPE;
    const GeasContractDesc* desc = attr_contract(rt, contract);
    if (!desc) return GEAS_ERR_NAME;
    return attr_find(desc, NULL, key, out);
}

/**
 * @params rt - the runtime, contract - its name, item - its member, key - the setting, out - the borrowed value
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_item_attr(GeasRuntime* rt, const char* contract, const char* item,
                          const char* key, const GeasValue** out) {
    if (!out) return GEAS_ERR_TYPE;
    *out = NULL;
    if (!rt || !contract || !item || !key) return GEAS_ERR_TYPE;
    const GeasContractDesc* desc = attr_contract(rt, contract);
    if (!desc) return GEAS_ERR_NAME;
    return attr_find(desc, item, key, out);
}

/**
 * @params c - the instance, count - the output count
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_instance_attr_count(const GeasContract* c, size_t* count) {
    if (!count) return GEAS_ERR_TYPE;
    *count = 0;
    if (!c) return GEAS_ERR_TYPE;
    *count = c->desc->nattrs;
    return GEAS_OK;
}

/**
 * @params c - the instance, index - the row, out - the borrowed row
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_instance_attr_at(const GeasContract* c, size_t index, GeasAttrInfo* out) {
    if (!out) return GEAS_ERR_TYPE;
    memset(out, 0, sizeof(*out));
    if (!c) return GEAS_ERR_TYPE;
    return attr_at(c->desc, index, out);
}

/**
 * @params c - the instance, item - a member or NULL, key - the setting, out - the borrowed value
 * @returns {GeasStatus} - the lookup status
 */
GeasStatus geas_instance_attr(const GeasContract* c, const char* item, const char* key,
                              const GeasValue** out) {
    if (!out) return GEAS_ERR_TYPE;
    *out = NULL;
    if (!c || !key) return GEAS_ERR_TYPE;
    return attr_find(c->desc, item, key, out);
}
