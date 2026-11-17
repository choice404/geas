// What the checker leaves behind for code generation: the type of every
// expression, what each name and call resolved to, and the workflow
// references the checker settled, each remedy's target and each after
// term's items. The tables index the program's tree, so they live and die
// with it
#pragma once

#include <cstdint>
#include <unordered_map>
#include <variant>
#include <vector>

#include "amenddiff.hpp"
#include "ast.hpp"
#include "course.hpp"
#include "types.hpp"

namespace geas {

// What an expression turned out to be, beyond its type. None covers every
// expression whose syntax already says it all
enum class ExprUse : std::uint8_t {
    None,
    Local,
    Vow,
    VariantValue,
    PriorInstance,
    ClauseCall,
    VariantCall,
    CoursePledgeCall,
    LenCall,
    NowCall,
    SignCall,
    ResumeCall,
    InstancePledgeCall,
    InstanceSignalCall,
    InstanceBuiltinCall,
    PledgeValueCall,
    StoreCall,
    InstanceVow,
    InstancePledgeValue,
    RecordField,
    PriorVow,
};

// An item an after term waits on
using OrderTarget = std::variant<const PledgeDecl*, const SignalDecl*, const SubcontractDecl*>;

struct CheckTables {
    // One entry per expression in the tree, unset where the checker never
    // typed the node, a schema name or a column in a Store call for instance
    std::vector<TyId> exprTypes;
    std::vector<ExprUse> exprUses;
    std::vector<TyId> patternTypes;
    std::unordered_map<const ContractDecl*, LoweredCourse> courses;
    std::unordered_map<const RemedyDecl*, const PledgeDecl*> remedyTargets;
    std::unordered_map<const Term*, std::vector<OrderTarget>> afterTargets;
    // The plan of every amends block whose recorded shape loaded
    std::unordered_map<const AmendsDecl*, AmendPlan> amends;
};

}  // namespace geas
