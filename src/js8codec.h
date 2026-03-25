#pragma once
/**
 * @file js8codec.h
 * @brief Internal JS8 encode/decode interface.
 *
 * The two Costas arrays and the encode() function are used by both the encoder
 * (api.cpp) and the decoder (js8codec.cpp).  The Decoder class is the
 * synchronous, Qt-free replacement for GFSK8::Decoder.
 */

#include "constants.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace gfsk8inner {

// ── Costas arrays ─────────────────────────────────────────────────────────────
// ORIGINAL: used by Normal (Normal) mode.
// MODIFIED: used by Fast, Turbo, Slow, Ultra modes.

namespace Costas {
    enum class Type { ORIGINAL, MODIFIED };
    using Array = std::array<std::array<int, 7>, 3>;

    // Returns a reference to the appropriate compile-time Costas array.
    Array const &get(Type type);
}

// ── Encode ───────────────────────────────────────────────────────────────────
// Port of the Fortran genjs8 subroutine.
// @param frameType  Lower 3 bits are the JS8 frame-type field.
// @param costas     3×7 Costas array for the selected submode.
// @param message    Pointer to exactly 12 JS8-alphabet characters.
// @param tones      Output: 79 integer tone values in [0, 7].
void encode(int frameType, Costas::Array const &costas,
            const char *message, int *tones);

// ── Decoder events ────────────────────────────────────────────────────────────
namespace Event {

struct DecodeStarted { int submodes; };
struct SyncStart     { int position; int size; };

struct SyncState {
    enum class Kind { CANDIDATE, DECODED } kind;
    int   mode;
    float frequency;
    float dt;
    union { int candidate; float decoded; } sync;
};

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

struct DecodeFinished { std::size_t count; };

using Variant = std::variant<DecodeStarted, SyncStart, SyncState,
                             Decoded, DecodeFinished>;
using Emitter = std::function<void(Variant const &)>;

} // namespace Event

// ── Synchronous decoder ───────────────────────────────────────────────────────
// Usage:
//   gfsk8inner::Decoder dec;
//   // populate g_decodeData (constants.h)
//   dec.snapshot();     // copy g_decodeData into decoder's private buffer
//   dec.decode(cb);     // run; cb invoked for every decoded frame
class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(Decoder const &) = delete;
    Decoder &operator=(Decoder const &) = delete;

    void snapshot();
    void decode(Event::Emitter const &emitter);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    DecodeData m_snapshot;  // private copy of g_decodeData
};

} // namespace gfsk8inner
