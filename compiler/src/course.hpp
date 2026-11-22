// The resumable course representation consumed by the emitter
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "types.hpp"

namespace geas {

enum class ExprUse : std::uint8_t;
struct CheckedProgram;

using CourseSlotId = NodeId<struct CourseSlotTag>;
using CourseLabelId = NodeId<struct CourseLabelTag>;

enum class CourseSlotKind { Local, Temporary, Collection, Position, Choice, Payload };
enum class CourseStepKind { Call, Await, Sleep, Select, Together };

struct CourseSlot {
    CourseSlotId id;
    std::string name;
    TyId type;
    CourseSlotKind kind;
    bool isMutable = false;
};

// A together call has no separate resume point; the group owns the poll
struct CourseCall {
    std::uint32_t pledge = 0;
    std::vector<ExprId> arguments;
    CourseSlotId result;
    bool hasQuestion = false;
};

// Signal indices include the descriptor's pledge count as their offset
struct CourseWaitArm {
    bool isTimeout = false;
    std::uint32_t signal = 0;
    std::int64_t milliseconds = 0;
    CourseSlotId payload;
};

struct CourseStep {
    std::uint32_t number = 0;
    CourseStepKind kind = CourseStepKind::Call;
    std::optional<std::uint32_t> member;
    std::vector<ExprId> arguments;
    CourseSlotId result;
    bool hasQuestion = false;
    std::optional<std::int64_t> withinMilliseconds;
    std::vector<CourseWaitArm> arms;
    std::vector<CourseCall> calls;
};

// A missing value writes Unit, otherwise Store copies the expression value
struct CourseStore {
    CourseSlotId slot;
    std::optional<ExprId> value;
};
struct CourseAssign { ExprId target; ExprId value; };
// Validate an evaluated lvalue path before later effects run; the final map
// index may insert a key, while every intermediate index must already exist
struct CourseCheckTarget { ExprId target; bool canInsert; };
struct CourseEvaluate { ExprId value; };
struct CourseResume { std::uint32_t step; };
struct CourseLabel { CourseLabelId label; };
struct CourseJump { CourseLabelId target; };
struct CourseBranch {
    ExprId condition;
    CourseLabelId whenTrue;
    CourseLabelId whenFalse;
};
struct CourseReturn {};

// A binding pattern copies its matched value into this frame slot
struct CoursePatternSlot { PatternId pattern; CourseSlotId slot; };
struct CourseMatchArm {
    PatternId pattern;
    CourseLabelId target;
    std::vector<CoursePatternSlot> bindings;
};
struct CourseMatch {
    ExprId value;
    std::vector<CourseMatchArm> arms;
};
struct CourseSelectArm {
    std::optional<PatternId> pattern;
    CourseLabelId target;
    std::vector<CoursePatternSlot> bindings;
};
struct CourseSelect {
    std::uint32_t step;
    std::vector<CourseSelectArm> arms;
};

// Tests the copied collection, copies its next element or key into variable,
// advances position, and jumps to body or done; continue jumps to this test
struct CourseForNext {
    CourseSlotId collection;
    CourseSlotId position;
    CourseSlotId variable;
    CourseLabelId body;
    CourseLabelId done;
};

using CourseInstruction = std::variant<CourseStore, CourseAssign, CourseCheckTarget, CourseEvaluate, CourseResume,
    CourseLabel, CourseJump, CourseBranch, CourseReturn, CourseMatch, CourseSelect, CourseForNext>;

struct LoweredCourse {
    // Expression and pattern ids address this arena, never the original AST
    // Literal patterns point to copied literals in the same expression arena
    Ast ast;
    std::vector<TyId> exprTypes;
    std::vector<ExprUse> exprUses;
    std::vector<CourseSlotId> exprSlots;
    std::vector<CourseSlot> slots;
    std::vector<CourseStep> steps;
    std::vector<CourseInstruction> instructions;
    std::uint64_t courseHash = 0;
};

/**
 * Lowers every checked course into the table keyed by its contract declaration
 * @params checked - the checked program and the table receiving its courses
 * @returns {std::expected<void, std::string>} - success or an unparkable hidden value diagnostic
 */
std::expected<void, std::string> lowerCourses(CheckedProgram& checked);

/**
 * Prints slots, source ordered resume steps, and lowered instructions
 * @params checked - the program containing the lowered courses
 * @returns {std::string} - a deterministic indented listing with a trailing newline
 */
std::string dumpCourses(const CheckedProgram& checked);

// courseHash is FNV-1a 64 of the canonical text of the lowered course, see
// CourseDumpMode in coursedumper.hpp, then the layout segments of every
// declared record and sum a slot type reaches, then the schema segments of
// the contract, see shape.hpp. Comments and spacing never reach it
// Steps are numbered in lexical preorder, before their operands and arms
// Execution follows instructions and can visit those numbers in another order
// A step result holds the raw operation value; Select holds the chosen arm
// index and copies the payload to that arm's typed slot before dispatch
// hasQuestion records syntax; Question expressions in instructions perform
// the unwrap, after the whole group completes for a Together step

}  // namespace geas
