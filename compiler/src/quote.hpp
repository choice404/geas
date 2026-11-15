// The small text helpers every diagnostic builder shares
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace geas {

// A name in single quotes, the way every diagnostic spells one
inline std::string quoted(std::string_view name) {
    std::string out{"'"};
    out.append(name);
    out.push_back('\'');
    return out;
}

// The fault for a labeled argument anywhere but a sign call
inline constexpr std::string_view labelFault = "named arguments are used only with sign";

// A count as decimal text
inline std::string countText(std::int64_t count) {
    return std::to_string(count);
}

// The opening of an argument fault, argument N then the tail, N counted
// from one
inline std::string argumentHead(std::size_t index, std::string_view tail) {
    return "argument " + std::to_string(index + 1) + std::string{tail};
}

}  // namespace geas
