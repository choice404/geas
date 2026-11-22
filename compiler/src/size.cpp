// Size decoding, see size.hpp
#include "size.hpp"

#include <limits>

#include "magnitude.hpp"

namespace geas {

namespace {

constexpr std::uint64_t decimalBase = 10;
constexpr std::uint64_t kibibyte = 1024;
constexpr std::uint64_t mebibyte = kibibyte * kibibyte;
constexpr std::uint64_t gibibyte = mebibyte * kibibyte;
constexpr std::uint64_t tebibyte = gibibyte * kibibyte;
constexpr std::uint64_t maxInt = std::numeric_limits<std::int64_t>::max();

std::uint64_t suffixFactor(int byte) {
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
    const auto magnitude = parseSizeMagnitude(literal);
    if (!magnitude) {
        return std::unexpected(magnitude.error());
    }
    if (*magnitude > maxInt) {
        return std::unexpected(SizeError::Overflow);
    }
    return static_cast<std::int64_t>(*magnitude);
}

std::expected<std::uint64_t, SizeError> parseSizeMagnitude(std::string_view literal) {
    std::uint64_t amount = 0;
    for (std::size_t index = 0; index < literal.size(); ++index) {
        const char byte = literal[index];
        if (byte == '_' && index > 0) {
            continue;
        }
        if (byte >= '0' && byte <= '9') {
            const auto digit = static_cast<std::uint64_t>(byte - '0');
            if (amount > (leastIntMagnitude - digit) / decimalBase) {
                return std::unexpected(SizeError::Overflow);
            }
            amount = amount * decimalBase + digit;
            continue;
        }
        const std::uint64_t factor = suffixFactor(byte);
        if (factor == 0 || index == 0 || index + 1 != literal.size()) {
            return std::unexpected(SizeError::StrayCharacter);
        }
        if (amount > leastIntMagnitude / factor) {
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
