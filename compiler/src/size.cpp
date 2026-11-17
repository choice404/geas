// Size decoding, see size.hpp
#include "size.hpp"

#include <limits>

namespace geas {

namespace {

constexpr std::int64_t decimalBase = 10;
constexpr std::int64_t kibibyte = 1024;
constexpr std::int64_t mebibyte = kibibyte * kibibyte;
constexpr std::int64_t gibibyte = mebibyte * kibibyte;
constexpr std::int64_t tebibyte = gibibyte * kibibyte;
constexpr std::int64_t maxInt = std::numeric_limits<std::int64_t>::max();

std::int64_t suffixFactor(int byte) {
    switch (byte) {
        case 'K': return kibibyte;
        case 'M': return mebibyte;
        case 'G': return gibibyte;
        case 'T': return tebibyte;
        default: return 0;
    }
}

}  // namespace

bool isSizeSuffix(int byte) {
    return suffixFactor(byte) != 0;
}

std::expected<std::int64_t, SizeError> parseSize(std::string_view literal) {
    std::int64_t amount = 0;
    for (std::size_t index = 0; index < literal.size(); ++index) {
        const char byte = literal[index];
        if (byte == '_' && index > 0) {
            continue;
        }
        if (byte >= '0' && byte <= '9') {
            const std::int64_t digit = byte - '0';
            if (amount > (maxInt - digit) / decimalBase) {
                return std::unexpected(SizeError::Overflow);
            }
            amount = amount * decimalBase + digit;
            continue;
        }
        const std::int64_t factor = suffixFactor(byte);
        if (factor == 0 || index == 0 || index + 1 != literal.size()) {
            return std::unexpected(SizeError::StrayCharacter);
        }
        if (amount > maxInt / factor) {
            return std::unexpected(SizeError::Overflow);
        }
        return amount * factor;
    }
    return std::unexpected(SizeError::StrayCharacter);
}

std::string_view sizeErrorMessage(SizeError error) {
    return error == SizeError::Overflow ? "size literal overflows 64 bits"
                                        : "invalid character in size literal";
}

}  // namespace geas
