// Big-endian loads/stores for ITCH wire format.
// memcpy into a local compiles to a single (unaligned-safe) load + bswap.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

namespace itch {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

[[nodiscard]] inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap16(v);
    return v;
}

[[nodiscard]] inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
    return v;
}

[[nodiscard]] inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap64(v);
    return v;
}

// ITCH timestamps are 6-byte nanoseconds since midnight.
[[nodiscard]] inline std::uint64_t load_be48(const std::uint8_t* p) noexcept {
    return (std::uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}

inline void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap16(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap64(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be48(std::uint8_t* p, std::uint64_t v) noexcept {
    store_be16(p, static_cast<std::uint16_t>(v >> 32));
    store_be32(p + 2, static_cast<std::uint32_t>(v));
}

}  // namespace itch
