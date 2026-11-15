// Loading a .geas file. A Source owns the whole file as one buffer, embedded
// NUL bytes included, plus the path it came from so a diagnostic can name
// its file. UTF-8 is a hard rule of the grammar, so a file that breaks it is
// refused here, before the lexer sees a byte
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace geas {

// A loaded source file, its path and its full text
struct Source {
    std::string path;
    std::string text;
};

// Why a source failed to load
enum class SourceError {
    CannotOpen,
    CannotRead,
    InvalidUTF8,
};

/**
 * Reads the file at path whole and checks that it is valid UTF-8.
 * @params path - the file to read
 * @returns {std::expected<Source, SourceError>} - the loaded source, or why it could not load
 */
std::expected<Source, SourceError> loadSource(std::string_view path);

/**
 * A plain description of a load failure, for the log line beside the
 * user facing message.
 * @params error - the failure to describe
 * @returns {std::string_view} - a static description
 */
std::string_view sourceErrorMessage(SourceError error);

}  // namespace geas
