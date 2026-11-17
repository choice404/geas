// Canonical type codes read back into the arena, see typecode.hpp
#include "typecode.hpp"

#include <array>

namespace geas {

namespace {

// The scalar kinds a one letter code can name
constexpr std::array<TyKind, 11> scalarKinds = {
    TyKind::Int,  TyKind::UInt,  TyKind::Float, TyKind::Bool,     TyKind::Byte, TyKind::Char,
    TyKind::String, TyKind::Bytes, TyKind::Unit, TyKind::Duration, TyKind::Time};

std::optional<TyKind> scalarOfCode(TypeArena& types, std::string_view code) {
    for (const TyKind kind : scalarKinds) {
        if (types.canon(types.scalar(kind)) == code) {
            return kind;
        }
    }
    return std::nullopt;
}

// A recursive descent over one canonical code, the inverse of the arena's
// canonical spelling
class CodeReader {
public:
    CodeReader(TypeArena& types, std::string_view code) : types_{types}, code_{code} {}

    std::optional<TyId> readAll() {
        const std::optional<TyId> type = read();
        return position_ == code_.size() ? type : std::nullopt;
    }

private:
    bool take(char c) {
        if (position_ < code_.size() && code_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }

    std::string readName() {
        const std::size_t start = position_;
        while (position_ < code_.size() && code_[position_] != ',' && code_[position_] != ')' &&
               code_[position_] != '-') {
            ++position_;
        }
        return std::string{code_.substr(start, position_ - start)};
    }

    // The comma separated types up to the closing parenthesis, which is taken
    std::optional<std::vector<TyId>> readList() {
        std::vector<TyId> types;
        if (take(')')) {
            return types;
        }
        do {
            const std::optional<TyId> type = read();
            if (!type) {
                return std::nullopt;
            }
            types.push_back(*type);
        } while (take(','));
        return take(')') ? std::optional{types} : std::nullopt;
    }

    std::optional<TyId> readComposite(char head, const std::vector<TyId>& args) {
        constexpr std::size_t pair = 2;
        switch (head) {
            case 'L': return args.size() == 1 ? std::optional{types_.list(args[0])} : std::nullopt;
            case 'O':
                return args.size() == 1 ? std::optional{types_.option(args[0])} : std::nullopt;
            case 'M':
                return args.size() == pair ? std::optional{types_.map(args[0], args[1])}
                                           : std::nullopt;
            case 'R':
                return args.size() == pair ? std::optional{types_.result(args[0], args[1])}
                                           : std::nullopt;
            default: return types_.tuple(args);
        }
    }

    std::optional<TyId> readPledge(const std::vector<TyId>& params) {
        if (!take('-') || !take('>')) {
            return std::nullopt;
        }
        const std::optional<TyId> ret = read();
        return ret ? std::optional{types_.pledge(params, *ret)} : std::nullopt;
    }

    std::optional<TyId> read() {
        if (position_ >= code_.size()) {
            return std::nullopt;
        }
        const char head = code_[position_++];
        if (head == 'N' || head == 'K') {
            const std::string name = readName();
            if (name.empty()) {
                return std::nullopt;
            }
            return head == 'N' ? types_.named(name) : types_.contract(name);
        }
        const std::string_view composites = "LOMRTP";
        if (composites.find(head) == std::string_view::npos) {
            const std::optional<TyKind> kind = scalarOfCode(types_, std::string_view{&head, 1});
            return kind ? std::optional{types_.scalar(*kind)} : std::nullopt;
        }
        if (!take('(')) {
            return std::nullopt;
        }
        const std::optional<std::vector<TyId>> args = readList();
        if (!args) {
            return std::nullopt;
        }
        return head == 'P' ? readPledge(*args) : readComposite(head, *args);
    }

    TypeArena& types_;
    std::string_view code_;
    std::size_t position_ = 0;
};

}  // namespace

std::optional<TyId> typeFromCode(TypeArena& types, std::string_view code) {
    return CodeReader{types, code}.readAll();
}

}  // namespace geas
