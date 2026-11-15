// Source loading, see source.hpp
#include "source.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <utility>

#include "utf8.hpp"

namespace geas {

namespace {

constexpr std::size_t readChunkSize = 64 * 1024;

// Closes the file when the handle leaves scope
struct FileCloser {
    void operator()(std::FILE* file) const { std::fclose(file); }
};

using FileHandle = std::unique_ptr<std::FILE, FileCloser>;

// Reads everything left in file, refusing on a read error such as a directory
std::expected<std::string, SourceError> readAll(std::FILE* file) {
    std::string contents;
    std::array<char, readChunkSize> chunk{};
    while (true) {
        const std::size_t count = std::fread(chunk.data(), 1, chunk.size(), file);
        contents.append(chunk.data(), count);
        if (count < chunk.size()) {
            break;
        }
    }
    if (std::ferror(file) != 0) {
        return std::unexpected(SourceError::CannotRead);
    }
    return contents;
}

}  // namespace

std::expected<Source, SourceError> loadSource(std::string_view path) {
    const std::string pathText{path};
    const FileHandle file{std::fopen(pathText.c_str(), "rb")};
    if (!file) {
        return std::unexpected(SourceError::CannotOpen);
    }
    auto contents = readAll(file.get());
    if (!contents) {
        return std::unexpected(contents.error());
    }
    if (!isValidUTF8(*contents)) {
        return std::unexpected(SourceError::InvalidUTF8);
    }
    return Source{pathText, std::move(*contents)};
}

std::string_view sourceErrorMessage(SourceError error) {
    switch (error) {
        case SourceError::CannotOpen: return "the file could not be opened";
        case SourceError::CannotRead: return "the file could not be read";
        case SourceError::InvalidUTF8: return "source is not valid UTF-8";
    }
    return "unknown load failure";
}

}  // namespace geas
