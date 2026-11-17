// The record command, see record.hpp
#include "record.hpp"

#include <cstdio>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "amendshape.hpp"
#include "check.hpp"
#include "emittext.hpp"
#include "quote.hpp"

namespace geas {

namespace {

constexpr int exitSuccess = 0;
constexpr int exitFailure = 1;
constexpr std::size_t readChunk = 4096;

void report(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
}

// What recording one contract comes to, judged before any file is written
enum class RecordAction { Write, Keep, Replace, Refuse };

struct RecordEntry {
    std::string contract;
    std::int64_t version;
    std::string path;
    std::string text;
    RecordAction action;
};

// The whole file at a path, nullopt when there is none to read
std::expected<std::string, std::string> readWhole(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::unexpected(joined("geas: cannot read ", geas::quoted(path)));
    }
    std::string text;
    std::vector<char> buffer(readChunk);
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
        text.append(buffer.data(), count);
    }
    const bool isRead = std::ferror(file) == 0;
    std::fclose(file);
    if (!isRead) {
        return std::unexpected(joined("geas: cannot read ", geas::quoted(path)));
    }
    return text;
}

bool writeWhole(const std::string& path, std::string_view text) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool isWritten = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool isClosed = std::fclose(file) == 0;
    return isWritten && isClosed;
}

// Whether a declaration's span sits inside the root file, the first file loaded
bool isInRootFile(const Program& program, const ContractDecl& contract) {
    if (program.files.empty()) {
        return false;
    }
    const LoadedFile& root = program.files.front();
    return contract.lo >= root.base && contract.lo <= root.base + root.length;
}

// Judges one entry against the file already at its path
std::expected<RecordAction, std::string> actionFor(const RecordEntry& entry, bool isForced) {
    if (!isRecordedShapeFile(entry.path)) {
        std::error_code error;
        const bool isTaken = std::filesystem::exists(entry.path, error);
        return isTaken ? std::expected<RecordAction, std::string>{std::unexpected(joined(
                             "geas: ", geas::quoted(entry.path), " is there but not a file"))}
                       : RecordAction::Write;
    }
    const auto existing = readWhole(entry.path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (*existing == entry.text) {
        return RecordAction::Keep;
    }
    return isForced ? RecordAction::Replace : RecordAction::Refuse;
}

// Every public contract of the root file, its recorded text, and what
// recording it comes to
std::expected<std::vector<RecordEntry>, std::string> planRecord(const CheckedProgram& checked,
                                                                std::string_view path,
                                                                bool isForced) {
    const std::string directory = directoryOfPath(path);
    std::vector<RecordEntry> entries;
    for (const ContractShape& contract : checked.contracts) {
        if (contract.decl->isInternal || !isInRootFile(checked.program, *contract.decl)) {
            continue;
        }
        const auto course = checked.tables.courses.find(contract.decl);
        const std::uint64_t courseHash =
            course == checked.tables.courses.end() ? 0 : course->second.courseHash;
        RecordEntry entry{contract.decl->name, contract.version,
                          recordedShapePath(directory, contract.decl->name, contract.version),
                          renderRecordedShape(
                              recordShape(contract, checked.sigs, checked.types, courseHash)),
                          RecordAction::Write};
        const auto action = actionFor(entry, isForced);
        if (!action) {
            return std::unexpected(action.error());
        }
        entry.action = *action;
        entries.push_back(std::move(entry));
    }
    return entries;
}

// Prints every refusal, true when there was one
bool reportRefusals(const std::vector<RecordEntry>& entries) {
    bool hasRefusal = false;
    for (const RecordEntry& entry : entries) {
        if (entry.action != RecordAction::Refuse) {
            continue;
        }
        hasRefusal = true;
        report(joined("geas: ", geas::quoted(entry.path), " already records a different shape of ",
                      geas::quoted(entry.contract), " version ",
                      std::to_string(entry.version),
                      "; nothing was written, record again with --force to replace it"));
    }
    return hasRefusal;
}

int writeEntries(const std::vector<RecordEntry>& entries) {
    for (const RecordEntry& entry : entries) {
        if (entry.action == RecordAction::Keep) {
            report(joined("[geas] ", entry.path, " already records this shape"));
            continue;
        }
        std::error_code error;
        std::filesystem::create_directories(directoryOfPath(entry.path), error);
        if (error || !writeWhole(entry.path, entry.text)) {
            report(joined("geas: could not write ", geas::quoted(entry.path)));
            return exitFailure;
        }
        const std::string_view verb =
            entry.action == RecordAction::Replace ? "[geas] replaced " : "[geas] recorded ";
        report(joined(verb, entry.path));
    }
    return exitSuccess;
}

}  // namespace

int runRecord(std::string_view path, bool isForced) {
    const auto checked = checkProgram(path);
    if (!checked) {
        for (const std::string& fault : checked.error()) {
            report(fault);
        }
        return exitFailure;
    }
    const auto entries = planRecord(*checked, path, isForced);
    if (!entries) {
        report(entries.error());
        return exitFailure;
    }
    if (entries->empty()) {
        report(joined("geas: ", geas::quoted(std::string{path}),
                      " declares no public contract to record"));
        return exitFailure;
    }
    if (reportRefusals(*entries)) {
        return exitFailure;
    }
    return writeEntries(*entries);
}

}  // namespace geas
