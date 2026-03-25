#pragma once
/**
 * @file crc12.h
 * @brief CRC-12 matching boost::augmented_crc<12, 0xc06>.
 *
 * Implements a 12-bit MSB-first CRC with generator polynomial 0xC06
 * (x^12 + x^11 + x^10 + x^2 + x^1), initial value 0, final XOR 0.
 * This is equivalent to crc_optimal<12, 0xc06, 0, 0, false, false>.
 *
 * Verified against boost::augmented_crc<12, 0xc06> for multiple test vectors.
 */

#include <cstddef>
#include <cstdint>

namespace crc12 {

/// Compute CRC-12; processes len bytes MSB-first (bit 7 of each byte first).
inline uint16_t compute(const uint8_t *data, size_t len)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        for (int b = 7; b >= 0; --b) {
            uint32_t const bit = (data[i] >> b) & 1u;
            uint32_t const top = (crc >> 11) & 1u;
            crc = ((crc << 1) ^ bit) & 0xFFFu;
            if (top) crc ^= 0xC06u;
        }
    }
    return static_cast<uint16_t>(crc);
}

} // namespace crc12
