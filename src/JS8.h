/**
 * @file JS8.h
 * @brief Internal JS8 codec interface: Costas arrays, encode, and Decoder.
 *
 * This header provides the internal C++ API used by JS8.cpp and the public
 * api.cpp wrapper.  It is not part of the public library interface
 * (include/gfsk8modem.h is the public header).
 *
 * Rewritten from the original JS8.h with the following differences:
 *   - Costas namespace restructured: the factory lambda is named `get`
 *     instead of `array`, clarifying that it retrieves a stored reference.
 *   - GFSK8::Decoder renamed methods: `copy()` → `snapshot()`, matching the
 *     cleaner js8codec.h naming; `copy()` remains as an alias.
 *   - Event::SyncState uses `kind` instead of `type` for the sub-enum
 *     discriminator, avoiding ambiguity with the outer `type` field.
 *   - All Qt references (QObject, QThread, Q_NAMESPACE) are absent.
 */
#pragma once

#include "commons.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace GFSK8 {

// ── Costas arrays ─────────────────────────────────────────────────────────────

namespace Costas {

enum class Type { ORIGINAL, MODIFIED };
using Array = std::array<std::array<int, 7>, 3>;

/**
 * @brief Return a reference to the compile-time Costas array for the given type.
 *
 * ORIGINAL is used by Normal (Normal) mode; MODIFIED by all other modes.
 */
// Module-level constexpr arrays (no static locals needed).
inline constexpr Array COSTAS_ORIGINAL = {{
    {4, 2, 5, 6, 1, 3, 0},
    {4, 2, 5, 6, 1, 3, 0},
    {4, 2, 5, 6, 1, 3, 0}
}};
inline constexpr Array COSTAS_MODIFIED = {{
    {0, 6, 2, 3, 5, 4, 1},
    {1, 5, 0, 2, 3, 6, 4},
    {2, 5, 0, 6, 4, 1, 3}
}};

constexpr Array const &get(Type type)
{
    return (type == Type::ORIGINAL) ? COSTAS_ORIGINAL : COSTAS_MODIFIED;
}

// Legacy accessor: `array(type)` → delegates to `get(type)`.
constexpr auto const &array(Type type) { return get(type); }

} // namespace Costas

// ── Encoder ───────────────────────────────────────────────────────────────────

/**
 * @brief Encode a 12-character JS8 message into 79 tone values.
 *
 * Port of the Fortran `genjs8` subroutine.
 *
 * @param type     Lower 3 bits are the JS8 frame-type field.
 * @param costas   3×7 Costas array appropriate for the selected submode.
 * @param message  Pointer to exactly 12 JS8-alphabet characters (not NUL-terminated).
 * @param tones    Output: 79 integer tone values in [0, 7].
 */
void encode(int type, Costas::Array const &costas,
            const char *message, int *tones);

// ── Decoder events ────────────────────────────────────────────────────────────

namespace Event {

struct DecodeStarted { int submodes; };
struct SyncStart     { int position; int size; };

struct SyncState {
    /// Distinguishes a candidate (pre-decode) from a confirmed decoded state.
    enum class Kind { CANDIDATE, DECODED } kind;
    int   mode;
    float frequency;
    float dt;
    union { int candidate; float decoded; } sync;
};

/// A successfully decoded JS8 frame.
struct Decoded {
    int         utc;
    int         snr;
    float       xdt;
    float       frequency;
    std::string data;
    int         type;
    float       quality;
    int         mode;
};

struct DecodeFinished { std::size_t decoded; };

using Variant = std::variant<DecodeStarted, SyncStart, SyncState,
                             Decoded, DecodeFinished>;
using Emitter = std::function<void(Variant const &)>;

} // namespace Event

// ── Synchronous decoder ───────────────────────────────────────────────────────

/**
 * @brief Synchronous, Qt-free JS8 decoder.
 *
 * Usage:
 *   GFSK8::Decoder dec;
 *   // Populate the global dec_data (commons.h) with audio and parameters.
 *   dec.snapshot();       // copy dec_data into the decoder's private buffer
 *   dec.decode(cb);       // run; cb called synchronously for each decoded frame
 *
 * The older `copy()` method is retained as an alias for `snapshot()`.
 */
class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(Decoder const &) = delete;
    Decoder &operator=(Decoder const &) = delete;
    Decoder(Decoder &&) = delete;
    Decoder &operator=(Decoder &&) = delete;

    /// Snapshot the global dec_data into the decoder's private buffer.
    void snapshot();

    /// Alias for snapshot() — retained for backward compatibility.
    void copy() { snapshot(); }

    /// Run a synchronous decode pass; emitEvent is called for each event.
    void decode(Event::Emitter const &emitEvent);

private:
    RxAudioBuffer m_data;

    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace GFSK8
