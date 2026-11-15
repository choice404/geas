// Byte level helpers the lexer, the diagnostics, and the dump all share. The
// front end works over byte offsets into the source, so every reader goes
// through byteAt, which hands back -1 past the end so a scan can peek ahead
// without a length guard at every step
#pragma once

#include <cstdint>
#include <string_view>

namespace geas {

// The value byteAt returns at or past either end of the buffer
inline constexpr int endOfInput = -1;

// A decoded UTF-8 scalar and the byte length of its encoding
struct DecodedScalar {
    std::int64_t scalar;
    std::int64_t length;
};

/**
 * The byte at index as an unsigned value, or endOfInput when out of range.
 * @params text - the buffer, index - the byte offset to read
 * @returns {int} - 0 to 255, or endOfInput
 */
int byteAt(std::string_view text, std::int64_t index);

/**
 * The byte length of a UTF-8 sequence given its lead byte. A byte that is not
 * a lead byte counts as one, so a scan always advances.
 * @params lead - the lead byte
 * @returns {std::int64_t} - 1 to 4
 */
std::int64_t utf8SequenceLength(int lead);

/**
 * The numeric value of one ASCII hex digit.
 * @params byte - the byte to read
 * @returns {int} - 0 to 15, or -1 when the byte is not a hex digit
 */
int hexValue(int byte);

/**
 * Whether byte is an ASCII hex digit, 0-9, a-f, or A-F.
 * @params byte - the byte to test
 * @returns {bool} - true for a hex digit
 */
bool isHexDigit(int byte);

/**
 * Decodes the scalar at pos. The source was validated at load, so the
 * continuation bytes are there; past the end reads as endOfInput of length 1.
 * @params text - the buffer, pos - the offset of the lead byte
 * @returns {DecodedScalar} - the scalar and its encoded length
 */
DecodedScalar decodeUTF8(std::string_view text, std::int64_t pos);

/**
 * Whether the whole buffer is well formed UTF-8: no bad lead byte, no missing
 * continuation, no overlong form, no surrogate, nothing above 0x10FFFF.
 * @params text - the buffer to check
 * @returns {bool} - true when every byte belongs to a valid scalar
 */
bool isValidUTF8(std::string_view text);

}  // namespace geas
