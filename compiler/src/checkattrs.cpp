// Attribute lists are literal host settings checked in their package scope
#include "checker.hpp"

#include "attributes.hpp"

namespace geas {

std::string_view attrTargetWord(AttrTarget target) {
    switch (target) {
        case AttrTarget::Contract: return "contract";
        case AttrTarget::Subcontract: return "subcontract";
        case AttrTarget::Pledge: return "pledge";
        case AttrTarget::Signal: return "signal";
        case AttrTarget::Remedy: return "remedy";
    }
    return "contract";
}

bool isBuiltinAttr(std::string_view key, AttrTarget target) {
    if (target == AttrTarget::Contract) {
        return key == "version" || key == "term";
    }
    if (target == AttrTarget::Pledge || target == AttrTarget::Remedy) {
        return key == "abi" || key == "symbol";
    }
    return false;
}

namespace {

bool isAttributeType(const Type& type) {
    const auto* named = std::get_if<NamedType>(&type);
    if (named == nullptr || !named->args.empty()) {
        return false;
    }
    return named->name == "Int" || named->name == "Float" || named->name == "Bool" ||
           named->name == "String" || named->name == "Duration";
}

void declareAttribute(const Program& program, const AttributeDecl& attribute,
                      int package, Resolved& resolved) {
    const auto fault = [&](std::string message) {
        resolved.diagnostics.push_back({attribute.lo, attribute.hi, std::move(message)});
    };
    if (isBuiltinAttr(attribute.name, attribute.target)) {
        fault("attribute " + quoted(attribute.name) + " shadows a builtin key on " +
              std::string{attrTargetWord(attribute.target)});
    }
    if (!isAttributeType(program.ast.type(attribute.type))) {
        fault("an attribute type is Int, Float, Bool, String, or Duration");
    }
    for (const AttributeEntry& entry : resolved.attributes) {
        if (entry.decl->name == attribute.name && entry.decl->target == attribute.target) {
            fault("duplicate attribute " + quoted(attribute.name) + " on " +
                  std::string{attrTargetWord(attribute.target)});
            return;
        }
    }
    resolved.attributes.push_back({&attribute, package});
}

}  // namespace

// Imported declarations carry the same package ids as every other top level name
void resolveAttributes(const Program& program, Resolved& resolved) {
    for (std::size_t i = 0; i < program.ast.decls.size(); ++i) {
        if (const auto* attribute = std::get_if<AttributeDecl>(&program.ast.decls[i])) {
            declareAttribute(program, *attribute, program.declPackages[i], resolved);
        }
    }
}

namespace {

std::string_view literalType(const Ast& ast, ExprId value) {
    const Expr* literal = &ast.expr(value);
    const auto* unary = std::get_if<UnaryExpr>(&literal->kind);
    const bool isNegative = unary != nullptr;
    if (isNegative) {
        literal = &ast.expr(unary->operand);
    }
    if (std::holds_alternative<IntLiteral>(literal->kind)) return "Int";
    if (std::holds_alternative<FloatLiteral>(literal->kind)) return "Float";
    if (std::holds_alternative<DurationLiteral>(literal->kind)) return "Duration";
    if (isNegative) return "invalid negative literal";
    if (std::holds_alternative<BoolLiteral>(literal->kind)) return "Bool";
    if (std::holds_alternative<StringLiteral>(literal->kind)) return "String";
    return "Char";
}

}  // namespace

/**
 * Resolves one declared setting and checks its literal against the scalar type
 * @params contract - the owning scope, attr - the setting, target - the declaration kind,
 * label - the declaration name used in diagnostics
 * @returns {void} - faults are recorded in the checker's diagnostics
 */
void Checker::checkAttribute(const ContractDecl& contract, const Attr& attr,
                             AttrTarget target, const std::string& label) {
    if (isBuiltinAttr(attr.key, target)) {
        return;
    }
    const int package = resolved_.contracts.at(&contract).package;
    bool hasOtherTarget = false;
    bool isInternal = false;
    for (const AttributeEntry& entry : resolved_.attributes) {
        if (entry.decl->name != attr.key) continue;
        if (entry.decl->isInternal && entry.package != package) {
            isInternal = true;
            continue;
        }
        if (entry.decl->target != target) {
            hasOtherTarget = true;
            continue;
        }
        const std::string& expected = std::get<NamedType>(ast_.type(entry.decl->type)).name;
        const std::string_view actual = literalType(ast_, attr.value);
        if (actual != expected) {
            emitAt(ast_.expr(attr.value), "attribute " + quoted(attr.key) + " expects " +
                   expected + ", found " + std::string{actual});
        }
        return;
    }
    unknownAttribute(attr, target, label, hasOtherTarget, isInternal);
}

void Checker::unknownAttribute(const Attr& attr, AttrTarget target, const std::string& label,
                               bool hasOtherTarget, bool isInternal) {
    std::string message = "unknown attribute key; the known keys are version, term, abi, and "
                          "symbol; declared attributes for this target are also accepted";
    if (attr.key == "term") {
        message = "the term attribute belongs on a contract, not on " + label;
    } else if (hasOtherTarget || attr.key == "version" || attr.key == "abi" ||
               attr.key == "symbol") {
        message = "attribute " + quoted(attr.key) + " does not target " +
                  std::string{attrTargetWord(target)};
    } else if (isInternal) {
        message = "internal attribute " + quoted(attr.key) + " is not visible outside its package";
    }
    emitAt(ast_.expr(attr.value), std::move(message));
}

/**
 * Checks each key once within one trailing list
 * @params contract - the owning scope, attrs - the list, target - the declaration kind,
 * label - the declaration name used in diagnostics
 * @returns {void} - duplicates and invalid settings are recorded as faults
 */
void Checker::checkAttrList(const ContractDecl& contract, const std::vector<Attr>& attrs,
                            AttrTarget target, const std::string& label) {
    std::unordered_set<std::string> seen;
    for (const Attr& attr : attrs) {
        if (!seen.insert(attr.key).second) {
            emitAt(ast_.expr(attr.value), "attribute " + quoted(attr.key) +
                   " appears more than once on " + label);
            continue;
        }
        checkAttribute(contract, attr, target, label);
    }
}

/**
 * Visits settings on a contract and all declarations inside it
 * @params contract - the contract whose settings are checked
 * @returns {void} - each list is checked in the contract's package scope
 */
void Checker::checkDeclaredAttrs(const ContractDecl& contract) {
    checkAttrList(contract, contract.attrs, AttrTarget::Contract, "contract " + quoted(contract.name));
    for (const Member& member : contract.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            checkAttrList(contract, pledge->attrs, AttrTarget::Pledge, "pledge " + quoted(pledge->name));
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            checkAttrList(contract, signal->attrs, AttrTarget::Signal, "signal " + quoted(signal->name));
        } else if (const auto* remedy = std::get_if<RemedyDecl>(&member)) {
            checkAttrList(contract, remedy->attrs, AttrTarget::Remedy, "remedy " + quoted(remedy->name));
        } else if (const auto* sub = std::get_if<SubcontractDecl>(&member)) {
            checkSubAttrs(contract, *sub);
        }
    }
}

/**
 * Checks the grouping settings and the pledge and signal lists inside it
 * @params contract - the owning scope, sub - the grouping declaration
 * @returns {void} - settings use the same package scope as the contract
 */
void Checker::checkSubAttrs(const ContractDecl& contract, const SubcontractDecl& sub) {
    const std::string label = sub.name ? "subcontract " + quoted(*sub.name) : "this subcontract";
    checkAttrList(contract, sub.attrs, AttrTarget::Subcontract, label);
    for (const SubMember& member : sub.members) {
        if (const auto* pledge = std::get_if<PledgeDecl>(&member)) {
            checkAttrList(contract, pledge->attrs, AttrTarget::Pledge, "pledge " + quoted(pledge->name));
        } else if (const auto* signal = std::get_if<SignalDecl>(&member)) {
            checkAttrList(contract, signal->attrs, AttrTarget::Signal, "signal " + quoted(signal->name));
        }
    }
}

}  // namespace geas
