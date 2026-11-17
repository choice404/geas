// Size literals count bytes in binary multiples of 1024
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace geas {

enum class SizeError { StrayCharacter, Overflow };

/**
 * Tests the byte immediately after a decimal integer
 * @params byte - the possible size suffix
 * @returns {bool} - true for K, M, G, or T
 */
bool isSizeSuffix(int byte);

/**
 * Decodes a decimal size literal into bytes
 * @params literal - the complete literal, including its suffix
 * @returns {std::expected<std::int64_t, SizeError>} - bytes, or the refusal
 */
std::expected<std::int64_t, SizeError> parseSize(std::string_view literal);

/**
 * Names a size literal refusal
 * @params error - the refusal to name
 * @returns {std::string_view} - the diagnostic message
 */
std::string_view sizeErrorMessage(SizeError error);

}  // namespace geas
