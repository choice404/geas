// The byte level helpers, see utf8.hpp
#include "utf8.hpp"

namespace geas {

namespace {

constexpr int asciiLimit = 0x80;
constexpr int continuationLimit = 0xC0;
constexpr int smallestTwoByteLead = 0xC2;
constexpr int threeByteLead = 0xE0;
constexpr int fourByteLead = 0xF0;
constexpr int pastFourByteLead = 0xF5;
constexpr int pastLeadByte = 0xF8;
constexpr int continuationMask = 0x3F;
constexpr int continuationBits = 6;
constexpr std::int64_t smallestThreeByteScalar = 0x800;
constexpr std::int64_t smallestFourByteScalar = 0x10000;
constexpr std::int64_t surrogateLow = 0xD800;
constexpr std::int64_t surrogateHigh = 0xDFFF;
constexpr std::int64_t maxScalar = 0x10FFFF;
constexpr int hexLetterOffset = 10;
constexpr std::int64_t maxSequenceLength = 4;

bool isContinuation(int byte) {
    return byte >= asciiLimit && byte < continuationLimit;
}

// The payload bits of the continuation byte at pos
std::int64_t continuationPayload(std::string_view text, std::int64_t pos) {
    return byteAt(text, pos) & continuationMask;
}

// Checks one multibyte sequence starting at pos and returns its length, or 0
// when the sequence is malformed
std::int64_t validSequenceLength(std::string_view text, std::int64_t pos) {
    const int lead = byteAt(text, pos);
    if (lead < smallestTwoByteLead || lead >= pastFourByteLead) {
        return 0;
    }
    const std::int64_t length = utf8SequenceLength(lead);
    for (std::int64_t offset = 1; offset < length; ++offset) {
        if (!isContinuation(byteAt(text, pos + offset))) {
            return 0;
        }
    }
    const std::int64_t scalar = decodeUTF8(text, pos).scalar;
    if (length == 3 && scalar < smallestThreeByteScalar) {
        return 0;
    }
    if (length == 3 && scalar >= surrogateLow && scalar <= surrogateHigh) {
        return 0;
    }
    if (length == 4 && (scalar < smallestFourByteScalar || scalar > maxScalar)) {
        return 0;
    }
    return length;
}

}  // namespace

int byteAt(std::string_view text, std::int64_t index) {
    if (index < 0 || index >= static_cast<std::int64_t>(text.size())) {
        return endOfInput;
    }
    return static_cast<unsigned char>(text[static_cast<std::size_t>(index)]);
}

std::int64_t utf8SequenceLength(int lead) {
    if (lead >= fourByteLead && lead < pastLeadByte) {
        return 4;
    }
    if (lead >= threeByteLead && lead < fourByteLead) {
        return 3;
    }
    if (lead >= continuationLimit && lead < threeByteLead) {
        return 2;
    }
    return 1;
}

int hexValue(int byte) {
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + hexLetterOffset;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + hexLetterOffset;
    }
    return -1;
}

bool isHexDigit(int byte) {
    return hexValue(byte) >= 0;
}

DecodedScalar decodeUTF8(std::string_view text, std::int64_t pos) {
    const int lead = byteAt(text, pos);
    if (lead < asciiLimit) {
        return DecodedScalar{lead, 1};
    }
    // A stray continuation byte decodes as a four byte form, only reachable
    // when an unknown escape splits a scalar
    const std::int64_t leadLength = utf8SequenceLength(lead);
    const std::int64_t length = leadLength == 1 ? maxSequenceLength : leadLength;
    // The lead keeps 7 minus length payload bits, 5 for two bytes down to 3 for four
    const int leadMask = (1 << (7 - length)) - 1;
    std::int64_t scalar = lead & leadMask;
    for (std::int64_t offset = 1; offset < length; ++offset) {
        scalar = (scalar << continuationBits) | continuationPayload(text, pos + offset);
    }
    return DecodedScalar{scalar, length};
}

bool isValidUTF8(std::string_view text) {
    const auto size = static_cast<std::int64_t>(text.size());
    std::int64_t pos = 0;
    while (pos < size) {
        if (byteAt(text, pos) < asciiLimit) {
            ++pos;
            continue;
        }
        const std::int64_t length = validSequenceLength(text, pos);
        if (length == 0) {
            return false;
        }
        pos += length;
    }
    return true;
}

}  // namespace geas
