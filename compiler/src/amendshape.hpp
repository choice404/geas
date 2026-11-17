// The recorded shape of one contract version, the file geas record writes
// beside the source as shapes/<Contract>.v<version>.shape and the checker
// reads back when a later version amends this one.
//
// The file is plain text, one record a line, every line ending in a newline:
//
//   geas shape 2
//   contract NAME
//   version N
//   hash 0x<16 hex digits>          the shape hash
//   course 0x<16 hex digits>        the course hash, all zero when there is none
//   shape <canonical shape string>
//   vow NAME TYPE                   one per vow, declaration order
//   pledge NAME SIGNATURE OK        one per pledge, descriptor order
//   signal NAME PAYLOAD             one per signal, after every pledge
//   sub NAME                        one per subcontract, a bare "sub" when anonymous
//   remedy NAME TARGET              one per remedy, declaration order
//   type NAME DEFINITION            one per declared record or sum the boundary reaches, name order
//
// Types are canonical type codes, the spelling shape hashes are taken over.
// A pledge's OK is the code of what a fulfilled pledge stores, the T of a
// Result<T, E> or the whole return otherwise, and a signal's PAYLOAD is its
// parameter list as a parenthesized code list. A code names a declared type
// and nothing more, so each record and sum the boundary reaches is recorded
// with its layout, the definition the shape string spells for it, see
// layout.hpp. The type records have to spell the layout segments the shape
// line ends in, which the hash covers. The first line names the format
// version, and a reader refuses a version it does not know
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace geas {

struct RecordedVow {
    std::string name;
    std::string type;
};

// A pledge or a signal, the rows of the descriptor's pledges table
struct RecordedItem {
    std::string name;
    bool isSignal;
    std::string signature;
    std::string value;
};

struct RecordedRemedy {
    std::string name;
    std::string target;
};

// A declared record or sum and its layout, {field:code,...} for a record and
// =Variant(field:code,...)|Variant for a sum
struct TypeLayout {
    std::string name;
    std::string definition;
};

struct RecordedShape {
    std::string contract;
    std::int64_t version = 0;
    std::uint64_t shapeHash = 0;
    std::uint64_t courseHash = 0;
    std::string shape;
    std::vector<RecordedVow> vows;
    std::vector<RecordedItem> items;
    std::vector<std::string> subs;
    std::vector<RecordedRemedy> remedies;
    std::vector<TypeLayout> layouts;
};

/**
 * The shape string segments of a list of layouts, one type segment each,
 * the tail of a shape string that reaches declared types, see layout.hpp.
 * @params layouts - the layouts, in name order
 * @returns {std::string} - the segments, empty when there are none
 */
std::string layoutSegments(const std::vector<TypeLayout>& layouts);

/**
 * Spells a recorded shape in the file format above.
 * @params shape - the shape to spell
 * @returns {std::string} - the file text, the same bytes for the same shape on every run
 */
std::string renderRecordedShape(const RecordedShape& shape);

/**
 * Reads a recorded shape back from its file text.
 * @params text - the whole file
 * @returns {std::expected<RecordedShape, std::string>} - the shape, or why the text is not one
 */
std::expected<RecordedShape, std::string> parseRecordedShape(std::string_view text);

/**
 * Reads and parses the recorded shape file at a path.
 * @params path - the .shape file
 * @returns {std::expected<RecordedShape, std::string>} - the shape, or why it could not be had
 */
std::expected<RecordedShape, std::string> readRecordedShape(const std::string& path);

/**
 * Whether a path names a regular file, the test that tells a shape never
 * recorded from one that cannot be read.
 * @params path - the .shape file
 * @returns {bool} - true when the file is there
 */
bool isRecordedShapeFile(const std::string& path);

/**
 * The path of a recorded shape: shapes/<Contract>.v<version>.shape under a directory.
 * @params directory - the directory the contract's source sits in, contract - its name,
 * version - the version recorded
 * @returns {std::string} - the path
 */
std::string recordedShapePath(std::string_view directory, std::string_view contract,
                              std::int64_t version);

/**
 * The directory part of a file path, "." when the path names no directory.
 * @params path - a file path
 * @returns {std::string} - the directory
 */
std::string directoryOfPath(std::string_view path);

}  // namespace geas
