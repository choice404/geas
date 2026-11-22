// The hash and the mangled name, see hash.hpp
#include "hash.hpp"

namespace geas {

namespace {

constexpr std::uint64_t fnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t fnvPrime = 0x100000001b3ULL;
constexpr int hexDigitCount = 16;
constexpr int bitsPerDigit = 4;
constexpr std::uint64_t digitMask = 0xF;
constexpr std::string_view hexDigits = "0123456789abcdef";
constexpr std::string_view manglePrefix = "__geas_ash_";

// One name with its byte length in front, so the name ends where the length
// says and no underscore inside it can run into the next segment
void appendLengthPrefixed(std::string& out, std::string_view name) {
    out.append(std::to_string(name.size()));
    out.append(name);
}

}  // namespace

std::uint64_t fnv1a64(std::string_view bytes) {
    std::uint64_t hash = fnvOffsetBasis;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= fnvPrime;
    }
    return hash;
}

std::string hashHex16(std::uint64_t hash) {
    std::string out;
    for (int digit = hexDigitCount - 1; digit >= 0; --digit) {
        out.push_back(hexDigits[(hash >> (bitsPerDigit * digit)) & digitMask]);
    }
    return out;
}

std::string mangleSymbol(std::string_view contract, std::string_view symbol, std::uint64_t sigHash,
                         std::int64_t version) {
    std::string out{manglePrefix};
    appendLengthPrefixed(out, contract);
    out.push_back('_');
    appendLengthPrefixed(out, symbol);
    out.push_back('_');
    out.append(hashHex16(sigHash));
    out.append("_v");
    out.append(std::to_string(version));
    return out;
}

}  // namespace geas
