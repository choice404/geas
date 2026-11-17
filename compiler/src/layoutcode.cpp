// The layouts a recorded code reaches, the recorded side, see layout.hpp
#include "layout.hpp"

namespace geas {

namespace {

constexpr char namedHead = 'N';
constexpr char contractHead = 'K';

// Whether a character ends a name inside a canonical code
bool isNameEnd(char c) {
    return c == ',' || c == ')' || c == '-';
}

// The declared names a canonical code spells, in the order they appear. A
// name follows an N or K head and runs to the next delimiter, and every
// other character of a code is a one letter head or punctuation
std::vector<std::string> namesOfCode(std::string_view code) {
    std::vector<std::string> names;
    std::size_t position = 0;
    while (position < code.size()) {
        const char head = code[position++];
        if (head != namedHead && head != contractHead) {
            continue;
        }
        const std::size_t start = position;
        while (position < code.size() && !isNameEnd(code[position])) {
            ++position;
        }
        if (head == namedHead) {
            names.emplace_back(code.substr(start, position - start));
        }
    }
    return names;
}

// The end of one field's code: the first comma, closing brace, or bar
// outside its parentheses, or the closing parenthesis of its variant
std::size_t fieldCodeEnd(std::string_view definition, std::size_t start) {
    int depth = 0;
    for (std::size_t i = start; i < definition.size(); ++i) {
        const char c = definition[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) {
                return i;
            }
            --depth;
        } else if (depth == 0 && (c == ',' || c == '}' || c == '|')) {
            return i;
        }
    }
    return definition.size();
}

// The code of every field a definition spells. A colon only ever separates
// a field name from its code, so each one opens a code
std::vector<std::string_view> fieldCodesOf(std::string_view definition) {
    std::vector<std::string_view> codes;
    std::size_t colon = definition.find(':');
    while (colon != std::string_view::npos) {
        const std::size_t start = colon + 1;
        const std::size_t end = fieldCodeEnd(definition, start);
        codes.push_back(definition.substr(start, end - start));
        colon = definition.find(':', end);
    }
    return codes;
}

const std::string* definitionOf(const std::vector<TypeLayout>& layouts, std::string_view name) {
    for (const TypeLayout& layout : layouts) {
        if (layout.name == name) {
            return &layout.definition;
        }
    }
    return nullptr;
}

// Every declared name a code reaches through a layout set, in name order
void reachCode(std::string_view code, const std::vector<TypeLayout>& layouts,
               std::set<std::string>& names) {
    for (std::string& name : namesOfCode(code)) {
        const std::string* definition = definitionOf(layouts, name);
        if (!names.insert(std::move(name)).second || definition == nullptr) {
            continue;
        }
        for (const std::string_view field : fieldCodesOf(*definition)) {
            reachCode(field, layouts, names);
        }
    }
}

}  // namespace

std::optional<std::string> changedLayout(std::string_view code,
                                         const std::vector<TypeLayout>& from,
                                         const std::vector<TypeLayout>& to) {
    std::set<std::string> names;
    reachCode(code, from, names);
    for (const std::string& name : names) {
        const std::string* was = definitionOf(from, name);
        const std::string* is = definitionOf(to, name);
        const bool isSame = (was == nullptr && is == nullptr) ||
                            (was != nullptr && is != nullptr && *was == *is);
        if (!isSame) {
            return name;
        }
    }
    return std::nullopt;
}

}  // namespace geas
