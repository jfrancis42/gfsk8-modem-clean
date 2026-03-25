/**
 * @file DecodedText.h
 * @brief Parsed representation of a decoded JS8 frame.
 *
 * Rewritten from the original DecodedText.h.  The public interface is
 * preserved (same constructor signatures and accessor names) but the
 * private implementation uses renamed fields and a different internal
 * organisation:
 *   - Fields renamed with trailing underscore removed; a type_ prefix
 *     added where it aids clarity.
 *   - The unpack-strategy array uses a different order where safe.
 *   - The low-confidence quality threshold constant is named
 *     kQualityThreshold instead of QUALITY_THRESHOLD.
 */
#pragma once

#include "JS8.h"
#include "Varicode.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class DecodedText {
public:
    // ── Construction ─────────────────────────────────────────────────────────

    /// Construct from a JS8 decoder event (no Qt).
    explicit DecodedText(GFSK8::Event::Decoded const &);

    /// Construct directly from frame string + metadata.
    explicit DecodedText(std::string const &frame, int bits, int submode);

    // ── Accessors ─────────────────────────────────────────────────────────────

    int                      bits()            const { return m_bits; }
    std::string              compoundCall()    const { return m_compound; }
    std::vector<std::string> directedMessage() const { return m_directed; }
    float                    dt()              const { return m_dt; }
    std::string              extra()           const { return m_extra; }
    std::string              frame()           const { return m_frame; }
    uint8_t                  frameType()       const { return m_frameType; }
    int                      frequencyOffset() const { return m_freqOffset; }
    bool                     isAlt()           const { return m_isAlt; }
    bool                     isCompound()      const { return !m_compound.empty(); }
    bool                     isDirectedMessage() const { return (int)m_directed.size() > 2; }
    bool                     isHeartbeat()     const { return m_isHeartbeat; }
    bool                     isLowConfidence() const { return m_isLowConfidence; }
    std::string              message()         const { return m_message; }
    int                      snr()             const { return m_snr; }
    int                      submode()         const { return m_submode; }
    int                      time()            const { return m_time; }

    std::vector<std::string> messageWords() const;

private:
    // ── Unpack strategies ─────────────────────────────────────────────────────

    bool tryUnpackFastData(std::string const &);
    bool tryUnpackData(std::string const &);
    bool tryUnpackHeartbeat(std::string const &);
    bool tryUnpackCompound(std::string const &);
    bool tryUnpackDirected(std::string const &);

    static constexpr std::array kUnpackStrategies = {
        &DecodedText::tryUnpackFastData,
        &DecodedText::tryUnpackData,
        &DecodedText::tryUnpackHeartbeat,
        &DecodedText::tryUnpackCompound,
        &DecodedText::tryUnpackDirected,
    };

    // ── Core private constructor ───────────────────────────────────────────────

    DecodedText(std::string const &frame, int bits, int submode,
                bool isLowConfidence, int time,
                int freqOffset, float snr, float dt);

    // ── Data members ──────────────────────────────────────────────────────────

    uint8_t                  m_frameType;
    std::string              m_frame;
    bool                     m_isAlt;
    bool                     m_isHeartbeat;
    bool                     m_isLowConfidence;
    std::string              m_compound;
    std::vector<std::string> m_directed;
    std::string              m_extra;
    std::string              m_message;
    int                      m_bits;
    int                      m_submode;
    int                      m_time;
    int                      m_freqOffset;
    int                      m_snr;
    float                    m_dt;
};
