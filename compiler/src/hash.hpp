// FNV-1a 64 over bytes, the one hash in the compiler, and the mangled name
// built from it. A contract's shape hash is the hash of its canonical shape
// string and a pledge's sig hash the hash of its canonical signature, so two
// builds of one source agree on every mangled name and every descriptor
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace geas {

/**
 * The FNV-1a 64 hash of a byte string.
 * @params bytes - the text to hash
 * @returns {std::uint64_t} - the hash
 */
std::uint64_t fnv1a64(std::string_view bytes);

/**
 * The 16 lowercase hex digits of a hash, high nibble first.
 * @params hash - the value to spell
 * @returns {std::string} - sixteen hex digits
 */
std::string hashHex16(std::uint64_t hash);

/**
 * The exported symbol name of a compiled pledge or remedy,
 * __geas_ash_{contract}_{symbol}_{sighash}_v{version}.
 * @params contract - the contract name, symbol - the pledge or remedy name, sigHash - the hash of
 * its canonical signature, version - the contract version
 * @returns {std::string} - the mangled name
 */
std::string mangleSymbol(std::string_view contract, std::string_view symbol, std::uint64_t sigHash,
                         std::int64_t version);

}  // namespace geas
