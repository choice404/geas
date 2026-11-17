// Internal writer for the lowered course listing
#pragma once

#include "check.hpp"
#include "course.hpp"

namespace geas {

class CourseDumper {
public:
    CourseDumper(const LoweredCourse& course, const TypeArena& types)
        : course_{course}, types_{types} {}
    std::string run(const std::string& name);

private:
    void line(int level, const std::string& text);
    std::string expression(ExprId value) const;
    std::string arguments(const std::vector<ExprId>& values) const;
    std::string callArguments(const std::vector<Argument>& values) const;
    std::string type(TypeId value) const;
    std::string pattern(PatternId value) const;
    std::string bindings(const std::vector<CoursePatternSlot>& values) const;
    void renderSlot(const CourseSlot& slot);
    void renderStep(const CourseStep& step);
    void renderCall(const CourseCall& call);
    void renderInstruction(const CourseInstruction& instruction);
    void renderMatch(const CourseMatch& match);
    void renderSelect(const CourseSelect& select);

    const LoweredCourse& course_;
    const TypeArena& types_;
    std::string output_;
};

}  // namespace geas
