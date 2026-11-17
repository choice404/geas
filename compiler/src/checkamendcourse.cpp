// The course rule of an amends block, checked once every course has lowered
// and its hash is known, see courseFault in amenddiff.hpp and
// checkAmendCourses in check.hpp
#include "amenddiff.hpp"
#include "check.hpp"

namespace geas {

std::optional<std::string> courseFault(const AmendPlan& plan, std::uint64_t courseHash,
                                       std::int64_t version) {
    const std::string from = "version " + std::to_string(plan.from.version);
    const std::string to = "version " + std::to_string(version);
    const bool hadCourse = plan.from.courseHash != 0;
    const bool hasCourse = courseHash != 0;
    const bool isChanged = hasCourse && plan.from.courseHash != courseHash;
    if (isChanged && !plan.isCourseRestarted) {
        return hadCourse ? "the course of " + to + " differs from the course of " + from +
                               ", so its frame cannot carry; add restart course"
                         : to + " adds a course " + from +
                               " did not have; add restart course to enter it at step 0";
    }
    if (isChanged || !plan.isCourseRestarted) {
        return std::nullopt;
    }
    if (hasCourse) {
        return "restart course, but the course is the one " + from +
               " had and its frame carries over";
    }
    return hadCourse ? "restart course, but " + to + " has no course; the frame of " + from +
                           " is dropped with no item"
                     : "restart course, but neither " + from + " nor " + to + " has a course";
}

std::vector<Diagnostic> checkAmendCourses(const CheckedProgram& checked) {
    std::vector<Diagnostic> faults;
    for (const ContractShape& contract : checked.contracts) {
        const auto course = checked.tables.courses.find(contract.decl);
        const std::uint64_t courseHash =
            course == checked.tables.courses.end() ? 0 : course->second.courseHash;
        for (const Member& member : contract.decl->members) {
            const auto* amends = std::get_if<AmendsDecl>(&member);
            const auto plan = amends == nullptr ? checked.tables.amends.end()
                                                : checked.tables.amends.find(amends);
            if (plan == checked.tables.amends.end()) {
                continue;
            }
            if (auto fault = courseFault(plan->second, courseHash, contract.version)) {
                faults.push_back(Diagnostic{amends->lo, amends->hi, std::move(*fault)});
            }
        }
    }
    return faults;
}

}  // namespace geas
