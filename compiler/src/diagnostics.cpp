// Diagnostic rendering, see diagnostics.hpp
#include "diagnostics.hpp"

#include <algorithm>

#include "utf8.hpp"

namespace geas {

namespace {

constexpr int asciiLimit = 0x80;
constexpr int continuationLimit = 0xC0;
constexpr std::string_view caretIndent = "  ";

// A resolved offset: its line and column plus the bounds of its line
struct ResolvedLocation {
    std::int64_t line;
    std::int64_t column;
    std::int64_t offset;
    std::int64_t lineStart;
    std::int64_t lineEnd;
};

std::int64_t textSize(std::string_view text) {
    return static_cast<std::int64_t>(text.size());
}

bool isScalarBoundary(std::string_view text, std::int64_t offset) {
    if (offset <= 0 || offset >= textSize(text)) {
        return true;
    }
    const int byte = byteAt(text, offset);
    return byte < asciiLimit || byte >= continuationLimit;
}

// Clamps an offset into the buffer and back to the nearest scalar boundary at
// or before it, so a caret never lands inside a multibyte character
std::int64_t clampToBoundary(std::string_view text, std::int64_t offset) {
    std::int64_t cursor = std::clamp<std::int64_t>(offset, 0, textSize(text));
    while (cursor > 0 && !isScalarBoundary(text, cursor)) {
        --cursor;
    }
    return cursor;
}

// Counts the scalars in [lo, hi), the unit columns and carets are measured in
std::int64_t countScalars(std::string_view text, std::int64_t lo, std::int64_t hi) {
    std::int64_t count = 0;
    std::int64_t cursor = lo;
    while (cursor < hi) {
        const int byte = byteAt(text, cursor);
        if (byte >= asciiLimit && byte < continuationLimit) {
            ++cursor;
            continue;
        }
        ++count;
        cursor += utf8SequenceLength(byte);
    }
    return count;
}

ResolvedLocation resolve(std::string_view text, std::int64_t rawOffset) {
    const std::int64_t offset = clampToBoundary(text, rawOffset);
    std::int64_t line = 1;
    std::int64_t lineStart = 0;
    for (std::int64_t i = 0; i < offset; ++i) {
        if (byteAt(text, i) == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    std::int64_t lineEnd = offset;
    while (lineEnd < textSize(text) && byteAt(text, lineEnd) != '\n') {
        ++lineEnd;
    }
    const std::int64_t column = countScalars(text, lineStart, offset) + 1;
    return ResolvedLocation{line, column, offset, lineStart, lineEnd};
}

// The padding that puts the caret under its column: a tab stays a tab so the
// terminal expands it the way it expanded the source line, any other scalar
// becomes one space
std::string caretPadding(std::string_view text, std::int64_t lo, std::int64_t hi) {
    std::string padding;
    std::int64_t cursor = lo;
    while (cursor < hi) {
        const int byte = byteAt(text, cursor);
        if (byte >= asciiLimit && byte < continuationLimit) {
            ++cursor;
            continue;
        }
        padding.push_back(byte == '\t' ? '\t' : ' ');
        cursor += utf8SequenceLength(byte);
    }
    return padding;
}

// The source line, then a caret at the fault column with tildes to the end of
// the span, clipped to the line
std::string caretBlock(std::string_view text, const ResolvedLocation& location,
                       std::int64_t hi) {
    const std::int64_t caretEnd = std::min(std::max(hi, location.offset), location.lineEnd);
    const std::int64_t width = std::max<std::int64_t>(countScalars(text, location.offset, caretEnd), 1);
    const auto lineStart = static_cast<std::size_t>(location.lineStart);
    const auto lineLength = static_cast<std::size_t>(location.lineEnd - location.lineStart);

    // The echoed line stops at an embedded NUL
    const std::string_view lineText = text.substr(lineStart, lineLength);
    std::string block{caretIndent};
    block.append(lineText.substr(0, lineText.find('\0')));
    block.push_back('\n');
    block.append(caretIndent);
    block.append(caretPadding(text, location.lineStart, location.offset));
    block.push_back('^');
    block.append(static_cast<std::size_t>(width - 1), '~');
    return block;
}

}  // namespace

LineColumn lineColumnAt(std::string_view text, std::int64_t offset) {
    const ResolvedLocation location = resolve(text, offset);
    return LineColumn{location.line, location.column};
}

std::string renderDiagnostic(std::string_view path, std::string_view text,
                             const Diagnostic& diagnostic) {
    const ResolvedLocation location = resolve(text, diagnostic.lo);
    const std::int64_t run = std::max<std::int64_t>(diagnostic.hi - diagnostic.lo, 0);
    const std::int64_t hi = clampToBoundary(text, diagnostic.lo + run);

    std::string rendered{path};
    rendered.append(": ");
    rendered.append(std::to_string(location.line));
    rendered.push_back(':');
    rendered.append(std::to_string(location.column));
    rendered.append(": error: ");
    rendered.append(diagnostic.message);
    rendered.push_back('\n');
    rendered.append(caretBlock(text, location, hi));
    return rendered;
}

}  // namespace geas
