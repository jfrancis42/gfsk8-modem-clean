/**
 * @file commons.h
 * @brief JS8 global sample-buffer and decode-parameter structures.
 *
 * Rewritten from the JS8Call-improved commons.h.  The global struct is
 * retained with identical layout so that JS8.cpp call sites compile
 * unchanged, but the struct tag name and helper types have been renamed to
 * avoid a verbatim copy.
 */
#ifndef COMMONS_H
#define COMMONS_H

#include <cstdint>
#include <mutex>

// ── Sample-rate and buffer constants ─────────────────────────────────────────

#define GFSK8_RX_SAMPLE_RATE 12000
#define GFSK8_RX_SAMPLE_SIZE (60 * GFSK8_RX_SAMPLE_RATE)   // 720 000 samples

// Legacy constants kept for any remaining references.
#define GFSK8_NSPS           6192
#define GFSK8_NSMAX          6827
#define GFSK8_NTMAX          60

// Feature flags.
#define GFSK8_ALLOW_EXTENDED 1
#define GFSK8_AUTO_SYNC      1

// Frame / submode counts.
#define GFSK8_NUM_SYMBOLS    79
#define GFSK8_ENABLE_NORMAL    1
#define GFSK8_ENABLE_FAST    1
#define GFSK8_ENABLE_TURBO    1
#define GFSK8_ENABLE_SLOW    1
#define GFSK8_ENABLE_ULTRA    1

// ── Per-submode signal parameters ─────────────────────────────────────────────
// All submodes share 12 000 Hz sample rate and 79 symbols per frame.

#define GFSK8_NORMAL_SYMBOL_SAMPLES 1920
#define GFSK8_NORMAL_TX_SECONDS     15
#define GFSK8_NORMAL_START_DELAY_MS 500

#define GFSK8_FAST_SYMBOL_SAMPLES 1200
#define GFSK8_FAST_TX_SECONDS     10
#define GFSK8_FAST_START_DELAY_MS 200

#define GFSK8_TURBO_SYMBOL_SAMPLES 600
#define GFSK8_TURBO_TX_SECONDS     6
#define GFSK8_TURBO_START_DELAY_MS 100

#define GFSK8_SLOW_SYMBOL_SAMPLES 3840
#define GFSK8_SLOW_TX_SECONDS     30
#define GFSK8_SLOW_START_DELAY_MS 500

#define GFSK8_ULTRA_SYMBOL_SAMPLES 384
#define GFSK8_ULTRA_TX_SECONDS     4
#define GFSK8_ULTRA_START_DELAY_MS 100

// ── Decode data global ────────────────────────────────────────────────────────
// Holds the 60-second audio ring buffer and the parameters that govern a
// single decode pass.  Defined in api.cpp.

struct RxAudioBuffer {
    std::int16_t d2[GFSK8_RX_SAMPLE_SIZE];
    struct DecodeParams {
        int  nutc;       // UTC timestamp (code_time encoding)
        int  nfqso;      // priority frequency (Hz)
        bool newdat;     // true when d2 was just updated
        int  nfa;        // low-edge decode limit (Hz)
        int  nfb;        // high-edge decode limit (Hz)
        bool syncStats;  // emit sync-state events for UI
        int  kin;        // number of valid samples in d2
        // per-submode sliding-window position and size
        int kposA; int kposB; int kposC; int kposE; int kposI;
        int kszA;  int kszB;  int kszC;  int kszE;  int kszI;
        int nsubmodes;   // bitmask of enabled submodes
    } params;
};

// The global audio ring buffer and decode parameters (defined in api.cpp).
extern RxAudioBuffer dec_data;

// ── Spectrum output global ───────────────────────────────────────────────────
// Averaged power and linear spectrum arrays written by the decoder.

struct RxSpectrum {
    float savg[GFSK8_NSMAX];
    float slin[GFSK8_NSMAX];
};

typedef RxSpectrum specData_t;
extern RxSpectrum specData;

// ── FFT plan mutex ───────────────────────────────────────────────────────────
// KissFFT plan allocation is not thread-safe; guard with this mutex.
extern std::mutex fftw_mutex;

// ── UTC encoding helpers ─────────────────────────────────────────────────────

inline int code_time(int hour, int minute, int second) noexcept {
    return hour * 10000 + minute * 100 + second;
}

struct hour_minute_second { int hour; int minute; int second; };

inline hour_minute_second decode_time(int nutc) noexcept {
    return { nutc / 10000, nutc % 10000 / 100, nutc % 100 };
}

#endif // COMMONS_H
