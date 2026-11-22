// Internal writer for the lowered course listing
#pragma once

#include <string>
#include <vector>

#include "check.hpp"
#include "course.hpp"

namespace geas {

// Listing is the geas dump-course text. Canonical is the text the course
// hash is taken over, which spells what the course does and nothing of how
// the source wrote it: no slot name, every pledge and signal by its name
// rather than its table index, every expression followed by its type code,
// every string literal led by its length so the text has one parse, every
// binding pattern by its position among the bindings of its arm rather
// than its name, every record literal's fields in declaration order, the
// order they are built in, every Float by the shortest decimal that reads
// back as its value, every constructor's type by the type code that follows
// it, and the labels of a variant's payload dropped, since they only repeat
// the field each position already names
enum class CourseDumpMode { Listing, Canonical };

class CourseDumper {
public:
    CourseDumper(const LoweredCourse& course, const TypeArena& types,
                 CourseDumpMode mode = CourseDumpMode::Listing,
                 std::vector<std::string> members = {}, const Sigs* sigs = nullptr)
        : course_{course}, types_{types}, mode_{mode}, members_{std::move(members)}, sigs_{sigs} {}
    std::string run(const std::string& name);

    /**
     * The canonical text of the lowered course, slots, steps, and statements.
     * @returns {std::string} - the text the course hash is taken over
     */
    std::string canonical();

private:
    bool isCanonical() const { return mode_ == CourseDumpMode::Canonical; }
    std::string memberText(std::uint32_t index) const;
    std::string typed(ExprId value, std::string text) const;
    void renderBody();
    void line(int level, const std::string& text);
    std::string expression(ExprId value) const;
    std::string arguments(const std::vector<ExprId>& values) const;
    std::string callArguments(const std::vector<Argument>& values, bool isLabeled = true) const;
    std::string call(const CallExpr& call) const;
    std::string recordLiteral(const RecordLiteral& record) const;
    std::string type(TypeId value) const;
    std::string pattern(PatternId value, const std::vector<CoursePatternSlot>& slots) const;
    std::string bindings(const std::vector<CoursePatternSlot>& values) const;
    void renderSlot(const CourseSlot& slot);
    void renderStep(const CourseStep& step);
    void renderCall(const CourseCall& call);
    void renderInstruction(const CourseInstruction& instruction);
    void renderMatch(const CourseMatch& match);
    void renderSelect(const CourseSelect& select);

    const LoweredCourse& course_;
    const TypeArena& types_;
    CourseDumpMode mode_;
    std::vector<std::string> members_;
    const Sigs* sigs_;
    std::string output_;
};

}  // namespace geas
