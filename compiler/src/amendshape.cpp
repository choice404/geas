// Recorded shapes, see amendshape.hpp
#include "amendshape.hpp"

#include <sys/stat.h>

#include <array>
#include <charconv>
#include <optional>
#include <utility>

#include "hash.hpp"
#include "source.hpp"

namespace geas {

namespace {

constexpr std::string_view formatLine = "geas shape 2";
constexpr std::string_view formatPrefix = "geas shape ";

// The format before type layouts were recorded, read only to name it
constexpr std::string_view layoutlessFormat = "1";
constexpr std::string_view shapesDirectory = "shapes";
constexpr std::string_view shapeExtension = ".shape";
constexpr std::string_view hexPrefix = "0x";
constexpr int hexBase = 16;
constexpr int decimalBase = 10;
constexpr std::size_t hexDigits = 16;
constexpr std::int64_t firstVersion = 1;

// What opens the first layout segment of a shape string; no other segment
// starts with the word type
constexpr std::string_view layoutMarker = ";type ";

// The header lines after the format line, in the order they must appear
enum HeaderField : std::size_t { ContractField, VersionField, HashField, CourseField, ShapeField };
constexpr std::array<std::string_view, 5> headerKeys = {"contract", "version", "hash", "course",
                                                        "shape"};
using HeaderValues = std::array<std::string_view, headerKeys.size()>;

// The record kinds of the body, in the order their groups must appear, and
// the number of words each carries after its key
enum RecordKind : std::size_t {
    VowRecord,
    PledgeRecord,
    SignalRecord,
    SubRecord,
    RemedyRecord,
    TypeRecord
};
constexpr std::array<std::string_view, 6> recordKeys = {"vow", "pledge", "signal",
                                                        "sub", "remedy", "type"};
constexpr std::array<std::size_t, recordKeys.size()> recordWords = {2, 3, 2, 1, 2, 2};

using Fault = std::unexpected<std::string>;

std::string hexText(std::uint64_t value) {
    return std::string{hexPrefix} + hashHex16(value);
}

// One line split at its first space into a key and the rest
std::pair<std::string_view, std::string_view> splitKey(std::string_view line) {
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return {line, std::string_view{}};
    }
    return {line.substr(0, space), line.substr(space + 1)};
}

// The words of a record's rest, split at single spaces
std::vector<std::string_view> splitWords(std::string_view rest) {
    std::vector<std::string_view> words;
    std::size_t start = 0;
    while (start <= rest.size()) {
        const std::size_t space = rest.find(' ', start);
        const std::size_t end = space == std::string_view::npos ? rest.size() : space;
        words.push_back(rest.substr(start, end - start));
        start = end + 1;
    }
    return words;
}

std::string lineFault(std::size_t index, std::string_view message) {
    return "line " + std::to_string(index + 1) + ": " + std::string{message};
}

std::optional<std::uint64_t> parseHex(std::string_view text) {
    if (!text.starts_with(hexPrefix) || text.size() != hexPrefix.size() + hexDigits) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* first = text.data() + hexPrefix.size();
    const char* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value, hexBase);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> parseVersion(std::string_view text) {
    std::int64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, decimalBase);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value < firstVersion) {
        return std::nullopt;
    }
    return value;
}

// The lines of a file that must end in a newline, the last empty piece dropped
std::expected<std::vector<std::string_view>, std::string> splitLines(std::string_view text) {
    if (text.empty() || text.back() != '\n') {
        return Fault{"the file does not end in a newline, so it was cut short"};
    }
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::expected<void, std::string> checkFormatLine(std::string_view line) {
    if (line == formatLine) {
        return {};
    }
    if (!line.starts_with(formatPrefix)) {
        return Fault{"the first line is not '" + std::string{formatLine} + "'"};
    }
    const std::string_view version = line.substr(formatPrefix.size());
    if (version == layoutlessFormat) {
        return Fault{"format version '1' records no type layouts, so this compiler no longer "
                     "reads it; run geas record on the source of that version again"};
    }
    const std::string_view current = formatLine.substr(formatPrefix.size());
    return Fault{"format version '" + std::string{version} +
                 "' is not one this compiler reads; it reads version " + std::string{current}};
}

// The header values, each line checked to carry its key in order
std::expected<HeaderValues, std::string> readHeader(const std::vector<std::string_view>& lines) {
    HeaderValues values{};
    for (std::size_t i = 0; i < headerKeys.size(); ++i) {
        const std::size_t index = i + 1;
        if (index >= lines.size()) {
            return Fault{"the header ends before its '" + std::string{headerKeys[i]} + "' line"};
        }
        const auto [key, rest] = splitKey(lines[index]);
        if (key != headerKeys[i] || rest.empty()) {
            const std::string want{headerKeys[i]};
            return Fault{lineFault(index, "expected the '" + want + "' line")};
        }
        values[i] = rest;
    }
    return values;
}

// The line index of a header field, the format line being line 0
std::size_t headerLine(HeaderField field) {
    return field + 1;
}

std::expected<void, std::string> fillHeader(const HeaderValues& values, RecordedShape& shape) {
    shape.contract = std::string{values[ContractField]};
    const std::optional<std::int64_t> version = parseVersion(values[VersionField]);
    const std::optional<std::uint64_t> hash = parseHex(values[HashField]);
    const std::optional<std::uint64_t> course = parseHex(values[CourseField]);
    if (!version) {
        return Fault{lineFault(headerLine(VersionField),
                               "the version is not a whole number of at least 1")};
    }
    if (!hash || !course) {
        return Fault{lineFault(headerLine(hash ? CourseField : HashField),
                               "a hash is not 0x and sixteen hex digits")};
    }
    shape.version = *version;
    shape.shapeHash = *hash;
    shape.courseHash = *course;
    shape.shape = std::string{values[ShapeField]};
    return {};
}

std::size_t rankOf(std::string_view key) {
    for (std::size_t i = 0; i < recordKeys.size(); ++i) {
        if (recordKeys[i] == key) {
            return i;
        }
    }
    return recordKeys.size();
}

void addRecord(std::size_t rank, const std::vector<std::string_view>& words,
               RecordedShape& shape) {
    const auto text = [&](std::size_t i) { return std::string{words[i]}; };
    switch (rank) {
        case VowRecord: shape.vows.push_back(RecordedVow{text(0), text(1)}); break;
        case PledgeRecord:
            shape.items.push_back(RecordedItem{text(0), false, text(1), text(2)});
            break;
        case SignalRecord: shape.items.push_back(RecordedItem{text(0), true, "", text(1)}); break;
        case SubRecord: shape.subs.push_back(text(0)); break;
        case RemedyRecord: shape.remedies.push_back(RecordedRemedy{text(0), text(1)}); break;
        default: shape.layouts.push_back(TypeLayout{text(0), text(1)}); break;
    }
}

// Every word of a record is filled, but a sub record's one word, which is
// empty for an anonymous subcontract
bool hasFilledWords(std::size_t rank, const std::vector<std::string_view>& words) {
    if (rank == SubRecord) {
        return true;
    }
    for (const std::string_view word : words) {
        if (word.empty()) {
            return false;
        }
    }
    return true;
}

// One body record, held to the group order and to its word count
std::expected<void, std::string> readRecord(std::string_view line, std::size_t index,
                                            std::size_t& lastRank, RecordedShape& shape) {
    const auto [key, rest] = splitKey(line);
    const std::size_t rank = rankOf(key);
    if (rank == recordKeys.size()) {
        return Fault{lineFault(index, "'" + std::string{key} + "' is not a record kind")};
    }
    if (rank < lastRank) {
        return Fault{lineFault(index, "a " + std::string{key} + " record is out of order")};
    }
    lastRank = rank;
    const std::vector<std::string_view> words = splitWords(rest);
    if (words.size() != recordWords[rank] || !hasFilledWords(rank, words)) {
        return Fault{lineFault(index, "the " + std::string{key} + " record has the wrong fields")};
    }
    addRecord(rank, words, shape);
    return {};
}

// The layout segments a shape line ends in, empty when it spells none
std::string_view layoutTail(std::string_view shape) {
    const std::size_t first = shape.find(layoutMarker);
    return first == std::string_view::npos ? std::string_view{} : shape.substr(first + 1);
}

}  // namespace

std::string layoutSegments(const std::vector<TypeLayout>& layouts) {
    std::string out;
    for (const TypeLayout& layout : layouts) {
        out.append("type " + layout.name + layout.definition + ";");
    }
    return out;
}

std::string renderRecordedShape(const RecordedShape& shape) {
    std::string out{formatLine};
    out.append("\ncontract " + shape.contract);
    out.append("\nversion " + std::to_string(shape.version));
    out.append("\nhash " + hexText(shape.shapeHash));
    out.append("\ncourse " + hexText(shape.courseHash));
    out.append("\nshape " + shape.shape);
    for (const RecordedVow& vow : shape.vows) {
        out.append("\nvow " + vow.name + " " + vow.type);
    }
    for (const RecordedItem& item : shape.items) {
        const std::string fields = item.isSignal ? item.value : item.signature + " " + item.value;
        out.append((item.isSignal ? "\nsignal " : "\npledge ") + item.name + " " + fields);
    }
    for (const std::string& sub : shape.subs) {
        out.append(sub.empty() ? "\nsub" : "\nsub " + sub);
    }
    for (const RecordedRemedy& remedy : shape.remedies) {
        out.append("\nremedy " + remedy.name + " " + remedy.target);
    }
    for (const TypeLayout& layout : shape.layouts) {
        out.append("\ntype " + layout.name + " " + layout.definition);
    }
    out.push_back('\n');
    return out;
}

std::expected<RecordedShape, std::string> parseRecordedShape(std::string_view text) {
    auto lines = splitLines(text);
    if (!lines) {
        return Fault{lines.error()};
    }
    if (auto format = checkFormatLine(lines->front()); !format) {
        return Fault{lineFault(0, format.error())};
    }
    const auto header = readHeader(*lines);
    if (!header) {
        return Fault{header.error()};
    }
    RecordedShape shape;
    if (auto filled = fillHeader(*header, shape); !filled) {
        return Fault{filled.error()};
    }
    std::size_t lastRank = 0;
    for (std::size_t i = headerKeys.size() + 1; i < lines->size(); ++i) {
        if (auto record = readRecord((*lines)[i], i, lastRank, shape); !record) {
            return Fault{record.error()};
        }
    }
    if (fnv1a64(shape.shape) != shape.shapeHash) {
        return Fault{"the hash is not the hash of the shape line, so the file was edited"};
    }
    if (layoutTail(shape.shape) != layoutSegments(shape.layouts)) {
        return Fault{"the type records are not the layouts the shape line spells, so the file "
                     "was edited"};
    }
    return shape;
}

std::expected<RecordedShape, std::string> readRecordedShape(const std::string& path) {
    const auto source = loadSource(path);
    if (!source) {
        return Fault{std::string{sourceErrorMessage(source.error())}};
    }
    return parseRecordedShape(source->text);
}

bool isRecordedShapeFile(const std::string& path) {
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return S_ISREG(info.st_mode);
}

std::string recordedShapePath(std::string_view directory, std::string_view contract,
                              std::int64_t version) {
    return std::string{directory} + "/" + std::string{shapesDirectory} + "/" +
           std::string{contract} + ".v" + std::to_string(version) + std::string{shapeExtension};
}

std::string directoryOfPath(std::string_view path) {
    const std::size_t last = path.rfind('/');
    if (last == std::string_view::npos) {
        return ".";
    }
    if (last == 0) {
        return "/";
    }
    return std::string{path.substr(0, last)};
}

}  // namespace geas
