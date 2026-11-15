// Duration decoding, see duration.hpp
#include "duration.hpp"

#include <array>
#include <limits>

namespace geas {

namespace {

constexpr std::int64_t decimalBase = 10;
constexpr std::int64_t maxInt = std::numeric_limits<std::int64_t>::max();

// A unit's spelling and how many milliseconds it is worth, largest first so
// the index doubles as the descending order rank
struct DurationUnit {
    std::string_view spelling;
    std::int64_t milliseconds;
};

constexpr std::int64_t millisecondsPerSecond = 1000;
constexpr std::int64_t millisecondsPerMinute = 60 * millisecondsPerSecond;
constexpr std::int64_t millisecondsPerHour = 60 * millisecondsPerMinute;
constexpr std::int64_t millisecondsPerDay = 24 * millisecondsPerHour;

constexpr std::array durationUnits{
    DurationUnit{"d", millisecondsPerDay},
    DurationUnit{"h", millisecondsPerHour},
    DurationUnit{"m", millisecondsPerMinute},
    DurationUnit{"s", millisecondsPerSecond},
    DurationUnit{"ms", 1},
};

constexpr std::size_t noUnitYet = durationUnits.size();

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// One part read off the front of the literal: its number, its unit rank, and
// how many bytes it took
struct DurationPart {
    std::int64_t amount;
    std::size_t unitRank;
    std::size_t length;
};

// Reads the decimal run at the front, underscores allowed after the first
// digit, refusing a value that cannot fit in 64 bits
std::expected<DurationPart, DurationError> readAmount(std::string_view text) {
    if (text.empty() || !isDigit(text.front())) {
        return std::unexpected(DurationError::StrayCharacter);
    }
    std::int64_t amount = 0;
    std::size_t length = 0;
    for (; length < text.size() && (isDigit(text[length]) || text[length] == '_'); ++length) {
        if (text[length] == '_') {
            continue;
        }
        const std::int64_t digit = text[length] - '0';
        if (amount > (maxInt - digit) / decimalBase) {
            return std::unexpected(DurationError::Overflow);
        }
        amount = amount * decimalBase + digit;
    }
    return DurationPart{amount, noUnitYet, length};
}

// The rank of a unit spelling, or noUnitYet when it is not a unit
std::size_t unitRank(std::string_view spelling) {
    for (std::size_t rank = 0; rank < durationUnits.size(); ++rank) {
        if (durationUnits[rank].spelling == spelling) {
            return rank;
        }
    }
    return noUnitYet;
}

// Reads one whole part, the amount then the letter run that names its unit
std::expected<DurationPart, DurationError> readPart(std::string_view text) {
    const auto amount = readAmount(text);
    if (!amount) {
        return amount;
    }
    std::size_t unitEnd = amount->length;
    while (unitEnd < text.size() && isLetter(text[unitEnd])) {
        ++unitEnd;
    }
    if (unitEnd == amount->length) {
        return std::unexpected(DurationError::MissingUnit);
    }
    const std::size_t rank = unitRank(text.substr(amount->length, unitEnd - amount->length));
    if (rank == noUnitYet) {
        return std::unexpected(DurationError::UnknownUnit);
    }
    return DurationPart{amount->amount, rank, unitEnd};
}

// Checks a part's unit against the one before it, descending and never repeated
std::expected<void, DurationError> checkOrder(std::size_t previousRank, std::size_t rank) {
    if (previousRank == noUnitYet) {
        return {};
    }
    if (rank == previousRank) {
        return std::unexpected(DurationError::RepeatedUnit);
    }
    if (rank < previousRank) {
        return std::unexpected(DurationError::OutOfOrder);
    }
    return {};
}

// The milliseconds a part is worth added onto total, refusing overflow
std::expected<std::int64_t, DurationError> addPart(std::int64_t total, const DurationPart& part) {
    const std::int64_t factor = durationUnits[part.unitRank].milliseconds;
    if (part.amount > maxInt / factor) {
        return std::unexpected(DurationError::Overflow);
    }
    const std::int64_t worth = part.amount * factor;
    if (total > maxInt - worth) {
        return std::unexpected(DurationError::Overflow);
    }
    return total + worth;
}

}  // namespace

bool isDurationUnitLead(int c) {
    return c == 'd' || c == 'h' || c == 'm' || c == 's';
}

std::expected<std::int64_t, DurationError> parseDuration(std::string_view literal) {
    std::int64_t total = 0;
    std::size_t previousRank = noUnitYet;
    std::string_view rest = literal;
    while (!rest.empty()) {
        const auto part = readPart(rest);
        if (!part) {
            return std::unexpected(part.error());
        }
        if (const auto order = checkOrder(previousRank, part->unitRank); !order) {
            return std::unexpected(order.error());
        }
        const auto sum = addPart(total, *part);
        if (!sum) {
            return sum;
        }
        total = *sum;
        previousRank = part->unitRank;
        rest.remove_prefix(part->length);
    }
    return total;
}

std::string_view durationErrorMessage(DurationError error) {
    switch (error) {
        case DurationError::UnknownUnit:
            return "unknown duration unit; expected d, h, m, s, or ms";
        case DurationError::MissingUnit: return "duration part is missing its unit";
        case DurationError::OutOfOrder:
            return "duration units must run from largest to smallest, d h m s ms";
        case DurationError::RepeatedUnit: return "duration repeats a unit";
        case DurationError::StrayCharacter: return "invalid character in duration literal";
        case DurationError::Overflow: return "duration literal overflows 64 bits";
    }
    return "invalid duration literal";
}

}  // namespace geas
