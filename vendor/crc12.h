#pragma once
// crc12.h — CRC-12 matching boost::augmented_crc<12, 0xc06>
//
// BUG FIX (2026-03-24):
//   The previous implementation used polynomial 0x80F with a flawed
//   byte-at-a-time algorithm and a final fold step.  It produced different
//   results from boost::augmented_crc<12, 0xc06> for all non-trivial inputs,
//   causing checkCRC12() to reject every correctly-decoded real JS8Call frame.
//   The loopback self-test still passed because both encoder and decoder called
//   the same (wrong) function, so the error cancelled out.
//
// CORRECT ALGORITHM:
//   boost::augmented_crc<12, 0xc06> is equivalent to crc_optimal<12, 0xc06,
//   0, 0, false, false>: a 12-bit, MSB-first, non-reflected CRC with
//   generator polynomial 0xC06 (x^12 + x^11 + x^10 + x^2 + x^1), initial
//   value 0, and final XOR 0.  Implemented here as a bit-at-a-time loop.
//   Verified against boost::augmented_crc<12, 0xc06> for multiple test vectors.

#include <cstddef>
#include <cstdint>

namespace crc12 {

// Compute CRC-12 matching boost::augmented_crc<12, 0xc06>.
// Input: raw bytes, MSB of each byte processed first.
// The input to JS8's CRC12() is an std::array<uint8_t, 11> (at most 88 bits),
// so the bit-at-a-time loop is negligible overhead.
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
