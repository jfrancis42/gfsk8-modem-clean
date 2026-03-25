#pragma once
/**
 * @file gfsk8modem.h
 * @brief Public C++ API for the gfsk8modem static library.
 *
 * gfsk8modem extracts the JS8 modem from JS8Call-improved and exposes it as a
 * dependency-free C++17 static library (libgfsk8modem.a).  No Qt, no FFTW3,
 * no Boost, no Eigen.
 *
 * License: GPL-3.0 — any project linking this library must also be GPL-3.0.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gfsk8 {

// ── Submodes ─────────────────────────────────────────────────────────────────

/**
 * JS8 submode.  The integer values match JS8Call-improved's Varicode::SubModeType
 * (Normal=0, Fast=1, Turbo=2, Slow=4, Ultra=8) and are safe to use as a
 * bitmask when selecting multiple modes for decoding.
 */
enum class Submode : int {
    Normal = 0,  ///< 15-second period, ~6.25 Hz tone spacing
    Fast   = 1,  ///< 10-second period, ~10.0 Hz tone spacing
    Turbo  = 2,  ///< 6-second period,  ~20.0 Hz tone spacing ("JS8 40")
    Slow   = 4,  ///< 30-second period, ~3.125 Hz tone spacing
    Ultra  = 8,  ///< 4-second period,  ~31.25 Hz tone spacing ("JS8 60")
};

/// Bitmask of all five submodes.
static constexpr int AllSubmodes = 0x1F;

/**
 * Fixed parameters for a submode (all derived from the JS8Call-improved
 * source; values are authoritative).
 */
struct SubmodeParms {
    int    samplesPerSymbol;  ///< audio samples per symbol at 12 kHz
    double toneSpacingHz;     ///< Hz between adjacent FSK tones
    int    periodSeconds;     ///< total TX/RX window in seconds
    int    startDelayMs;      ///< silence before first symbol (ms)
    int    numSymbols;        ///< always 79
    int    sampleRate;        ///< always 12000 Hz
    int    rxSnrThresholdDb;  ///< typical decode threshold in dB
};

/// Return the fixed parameters for a submode.
SubmodeParms submodeParms(Submode s);

// ── TX ────────────────────────────────────────────────────────────────────────

/**
 * Encode a JS8 message string into 79 8-FSK tone values (0–7).
 *
 * The message must be exactly 12 characters from the JS8 alphabet.  Call
 * sites that build the 12-character encoded string from a human-readable
 * message should use the JS8Call message-packing layer (not included in
 * this library — this function operates at the physical-layer level).
 *
 * @param submode   Submode (selects the Costas array).
 * @param frameType Lower 3 bits; use 0 for data frames.
 * @param message   Exactly 12-character JS8-alphabet string.
 * @param tonesOut  Output: 79 tone values in [0,7].
 * @return false if the message contains invalid characters.
 */
bool encode(Submode submode, int frameType,
            std::string_view message,
            std::vector<int> &tonesOut);

/**
 * Encode and modulate a message to PCM float32 audio at 12 000 Hz.
 *
 * The returned vector includes the start-delay silence prefix.  Returns an
 * empty vector on error.
 *
 * @param submode          Submode.
 * @param frameType        Lower 3 bits; use 0 for data frames.
 * @param message          Exactly 12-character JS8-alphabet string.
 * @param audioFrequencyHz Carrier frequency in Hz (default 1500 Hz).
 */
std::vector<float> modulate(Submode submode, int frameType,
                             std::string_view message,
                             double audioFrequencyHz = 1500.0);

// ── Message packing ───────────────────────────────────────────────────────────

/// A single packed physical-layer TX frame, ready for modulate().
struct TxFrame {
    std::string payload;   ///< exactly 12-char JS8-alphabet payload
    int         frameType; ///< frame type bits (Varicode::TransmissionType)
};

/**
 * Pack a human-readable JS8Call message into physical-layer frames.
 *
 * Handles all message types: heartbeat, directed, compound, data.
 * Long messages are automatically split into multiple frames, each of
 * which should be transmitted in a successive period.
 *
 * @param mycall   Sender's callsign (e.g. "W5XYZ")
 * @param mygrid   Sender's 4- or 6-char grid square (e.g. "DM79AA")
 * @param text     Human-readable message (e.g. "@APRSIS GRID DM79AA")
 * @param submode  Submode (affects data-frame packing)
 * @return         Ordered list of frames to transmit; empty on failure
 */
std::vector<TxFrame> pack(std::string const &mycall,
                          std::string const &mygrid,
                          std::string const &text,
                          Submode submode = Submode::Normal);

// ── RX ───────────────────────────────────────────────────────────────────────

/**
 * A successfully decoded JS8 frame.
 */
struct Decoded {
    std::string message;    ///< 12-character decoded payload
    int    snrDb;           ///< signal-to-noise ratio (dB)
    float  frequencyHz;     ///< estimated signal frequency (Hz)
    float  dtSeconds;       ///< timing offset from period start (s)
    int    submode;         ///< Submode enum value
    float  quality;         ///< decoder quality metric
    int    frameType;       ///< frame type (lower 3 bits of type field)
    int    utc;             ///< UTC timestamp (code_time() encoding)
};

using DecodeCallback = std::function<void(Decoded const &)>;

/**
 * Opaque decoder context.  Create one instance per decode thread.
 *
 * The decoder is heavy: it allocates FFT plans for all enabled submodes
 * on construction.  Reuse it across multiple decode periods rather than
 * creating a new one each time.
 *
 * Thread safety: a single Decoder must not be called concurrently.
 */
class Decoder {
public:
    /**
     * Create a decoder for the specified submodes.
     *
     * @param submodes  Bitmask of Submode values.  Use AllSubmodes (0x1F)
     *                  to enable all five modes simultaneously.
     * @param nfa       Low-frequency decode limit in Hz  (default 200).
     * @param nfb       High-frequency decode limit in Hz (default 4000).
     */
    explicit Decoder(int submodes = AllSubmodes, int nfa = 200, int nfb = 4000);
    ~Decoder();

    Decoder(Decoder const &) = delete;
    Decoder &operator=(Decoder const &) = delete;
    Decoder(Decoder &&) = delete;
    Decoder &operator=(Decoder &&) = delete;

    /**
     * Feed one period's worth of 12 kHz, int16 audio samples.
     *
     * The callback is invoked synchronously (on the calling thread) once
     * for each decoded frame found in the supplied audio block.
     *
     * @param samples   PCM int16 audio at 12 000 Hz.  The length must be
     *                  at least samplesPerPeriod for the slowest enabled
     *                  submode; pass GFSK8_RX_SAMPLE_SIZE (720 000) for the
     *                  60-second ring-buffer size used by JS8Call.
     * @param nutc      UTC timestamp in code_time() format.
     * @param cb        Called once per decoded frame.
     */
    void decode(std::span<std::int16_t const> samples,
                int nutc,
                DecodeCallback const &cb);

    /// Reset internal state (clears soft-combining buffers).
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ── Logging ──────────────────────────────────────────────────────────────────

/**
 * Set an optional log callback for internal diagnostic messages.
 *
 * If not set, all log output is silently discarded.  Set once at startup
 * before using any other library function.  The callback must be
 * thread-safe if the decoder is used from multiple threads.
 *
 * @param cb  Callback receiving NUL-terminated log message strings.
 */
void setLogCallback(std::function<void(const char *)> cb);

} // namespace gfsk8
