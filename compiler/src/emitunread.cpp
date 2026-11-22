// The unread local pass, see emitunread.hpp
//
// The pass reads the body as text, so it relies on the shapes the emitter
// writes: a temporary is declared as "GeasValue tN = ...;" on a line of its
// own, and a line that stores into one starts with the temporary or one of
// its fields followed by " = ". Every other mention of the name is a read,
// taking its address and passing it to a call among them
#include "emitunread.hpp"

#include <cctype>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace geas {
namespace {

constexpr std::string_view declarationHead = "GeasValue ";
constexpr std::string_view assignmentMark = " = ";
constexpr std::string_view discardHead = "(void)";

// The body split at its newlines, each line without its newline
std::vector<std::string_view> splitLines(std::string_view body) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find('\n', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        lines.push_back(body.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

bool isIdentifierByte(char byte) {
    return std::isalnum(static_cast<unsigned char>(byte)) != 0 || byte == '_';
}

// The leading spaces of one line
std::string_view indentOf(std::string_view line) {
    std::size_t width = 0;
    while (width < line.size() && line[width] == ' ') {
        ++width;
    }
    return line.substr(0, width);
}

// The temporary a line declares as a single GeasValue, empty for any other
// line, an array declaration among them
std::string_view declaredTemporary(std::string_view line) {
    std::string_view rest = line.substr(indentOf(line).size());
    if (rest.substr(0, declarationHead.size()) != declarationHead) {
        return {};
    }
    rest.remove_prefix(declarationHead.size());
    if (rest.empty() || rest[0] != 't') {
        return {};
    }
    std::size_t width = 1;
    while (width < rest.size() && std::isdigit(static_cast<unsigned char>(rest[width])) != 0) {
        ++width;
    }
    if (width == 1 || rest.substr(width, assignmentMark.size()) != assignmentMark) {
        return {};
    }
    return rest.substr(0, width);
}

// Whether a whole identifier has the form of a temporary, 't' then digits
bool isTemporaryName(std::string_view word) {
    if (word.size() < 2 || word[0] != 't') {
        return false;
    }
    for (std::size_t i = 1; i < word.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(word[i])) == 0) {
            return false;
        }
    }
    return true;
}

// Whether the mention at offset is the target of a store that opens the
// line: the name, then any run of field selections, then " = "
bool isStoreTarget(std::string_view line, std::size_t offset, std::string_view name) {
    if (offset != indentOf(line).size()) {
        return false;
    }
    std::size_t at = offset + name.size();
    while (at < line.size() && (line[at] == '.' || isIdentifierByte(line[at]))) {
        ++at;
    }
    return line.substr(at, assignmentMark.size()) == assignmentMark;
}

// Every whole identifier of a line that could name a temporary, a 't'
// followed by digits, with its offset
template <typename Visit>
void forEachTemporaryMention(std::string_view line, Visit&& visit) {
    std::size_t at = 0;
    while (at < line.size()) {
        if (!isIdentifierByte(line[at])) {
            ++at;
            continue;
        }
        const std::size_t start = at;
        while (at < line.size() && isIdentifierByte(line[at])) {
            ++at;
        }
        const std::string_view word = line.substr(start, at - start);
        if (isTemporaryName(word)) {
            visit(word, start);
        }
    }
}

// How many mentions of name in one line are reads rather than store targets
std::size_t readsInLine(std::string_view line, std::string_view name) {
    std::size_t reads = 0;
    forEachTemporaryMention(line, [&](std::string_view word, std::size_t offset) {
        if (word == name && !isStoreTarget(line, offset, word)) {
            ++reads;
        }
    });
    return reads;
}

// The reads of every temporary across the whole body, counted in one pass
std::unordered_map<std::string_view, std::size_t> countReads(
    const std::vector<std::string_view>& lines) {
    std::unordered_map<std::string_view, std::size_t> reads;
    for (const std::string_view line : lines) {
        forEachTemporaryMention(line, [&](std::string_view word, std::size_t offset) {
            if (!isStoreTarget(line, offset, word)) {
                ++reads[word];
            }
        });
    }
    return reads;
}

}  // namespace

// Two passes: the first counts the reads of every temporary over the whole
// body, the second writes the body and a (void) line after each declaration
// whose temporary no other line reads
std::string markUnreadLocals(std::string_view body) {
    const std::vector<std::string_view> lines = splitLines(body);
    const std::unordered_map<std::string_view, std::size_t> reads = countReads(lines);
    std::string marked;
    marked.reserve(body.size());
    for (const std::string_view line : lines) {
        marked.append(line);
        marked.push_back('\n');
        const std::string_view name = declaredTemporary(line);
        if (name.empty()) {
            continue;
        }
        const auto found = reads.find(name);
        const std::size_t total = found == reads.end() ? 0 : found->second;
        if (total > readsInLine(line, name)) {
            continue;
        }
        marked.append(indentOf(line));
        marked.append(discardHead);
        marked.append(name);
        marked.append(";\n");
    }
    return marked;
}

}  // namespace geas
