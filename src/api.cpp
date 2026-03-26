/**
 * @file api.cpp
 * @brief Implementation of the public gfsk8modem C++ API.
 *
 * Defines the global variables declared extern in commons.h:
 *   RxAudioBuffer dec_data;
 *   RxSpectrum    specData;
 *   std::mutex    fftw_mutex;
 *
 * Also defines the gfsk8::LogDispatcher g_logger global declared in log.h.
 *
 * Rewritten from the original api.cpp; the externally visible API
 * (include/gfsk8modem.h) is unchanged.  Internal differences:
 *   - Globals use the new struct types (RxAudioBuffer, RxSpectrum).
 *   - Log state is held in a LogDispatcher struct (g_logger) instead of a
 *     bare std::function<> (g_logCallback).
 *   - gfsk8::Decoder::decode() calls GFSK8::Decoder::snapshot() instead of
 *     the legacy GFSK8::Decoder::copy().
 */

#include "gfsk8modem.h"

#include "commons.h"
#include "JS8.h"
#include "JS8Submode.h"
#include "Varicode.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <stdexcept>

// ── Global definitions (declared extern in commons.h / log.h) ─────────────────

RxAudioBuffer dec_data;
RxSpectrum    specData;
std::mutex    fftw_mutex;

namespace gfsk8 {

LogDispatcher g_logger;

// ── Logging callback ──────────────────────────────────────────────────────────

void setLogCallback(std::function<void(const char *)> cb) {
    g_logger.callback = std::move(cb);
}

// ── SubmodeParms ─────────────────────────────────────────────────────────────

SubmodeParms submodeParms(Submode s) {
    int const sm = static_cast<int>(s);
    return SubmodeParms{
        static_cast<int>(GFSK8::Submode::samplesForOneSymbol(sm)),
        GFSK8::Submode::toneSpacing(sm),
        static_cast<int>(GFSK8::Submode::period(sm)),
        static_cast<int>(GFSK8::Submode::startDelayMS(sm)),
        GFSK8_NUM_SYMBOLS,
        GFSK8_RX_SAMPLE_RATE,
        GFSK8::Submode::rxSNRThreshold(sm),
    };
}

// ── Encode ───────────────────────────────────────────────────────────────────

bool encode(Submode submode, int frameType,
            std::string_view message,
            std::vector<int> &tonesOut)
{
    if (message.size() != 12) return false;

    int const sm          = static_cast<int>(submode);
    auto const &costasArr = GFSK8::Costas::get(GFSK8::Submode::costas(sm));

    tonesOut.resize(GFSK8_NUM_SYMBOLS);
    try {
        GFSK8::encode(frameType, costasArr, message.data(), tonesOut.data());
    } catch (...) {
        return false;
    }
    return true;
}

// ── Modulate ─────────────────────────────────────────────────────────────────

std::vector<float> modulate(Submode submode, int frameType,
                             std::string_view message,
                             double audioFrequencyHz)
{
    std::vector<int> tones;
    if (!encode(submode, frameType, message, tones))
        return {};

    int const sm        = static_cast<int>(submode);
    int const sps       = static_cast<int>(GFSK8::Submode::samplesForOneSymbol(sm));
    double const df     = GFSK8::Submode::toneSpacing(sm);
    int const delaySamp = static_cast<int>(
        GFSK8::Submode::startDelayMS(sm) * GFSK8_RX_SAMPLE_RATE / 1000.0 + 0.5);

    int const totalSamples = delaySamp + GFSK8_NUM_SYMBOLS * sps;
    std::vector<float> pcm(static_cast<std::size_t>(totalSamples), 0.0f);

    double phase = 0.0;
    int idx = delaySamp;

    for (int sym = 0; sym < GFSK8_NUM_SYMBOLS; ++sym) {
        double const freq = audioFrequencyHz + tones[sym] * df;
        double const dphi = 2.0 * std::numbers::pi * freq / GFSK8_RX_SAMPLE_RATE;

        for (int i = 0; i < sps; ++i, ++idx) {
            pcm[static_cast<std::size_t>(idx)] = static_cast<float>(std::sin(phase));
            phase += dphi;
            if (phase > std::numbers::pi) phase -= 2.0 * std::numbers::pi;
        }
    }

    return pcm;
}

// ── Message packing ──────────────────────────────────────────────────────────

std::vector<TxFrame> pack(std::string const &mycall,
                          std::string const &mygrid,
                          std::string const &text,
                          Submode submode)
{
    int const sm = static_cast<int>(submode);
    auto raw = Varicode::buildMessageFrames(
        mycall, mygrid, /*selectedCall=*/"", text,
        /*forceIdentify=*/false, /*forceData=*/false, sm, nullptr);

    std::vector<TxFrame> result;
    result.reserve(raw.size());
    for (auto const &[payload, ft] : raw)
        result.push_back({payload, ft});
    return result;
}

// ── Decoder ──────────────────────────────────────────────────────────────────

struct Decoder::Impl {
    int submodes;
    int nfa;
    int nfb;
    std::unique_ptr<GFSK8::Decoder> core;

    Impl(int sm, int lo, int hi)
        : submodes(sm), nfa(lo), nfb(hi),
          core(std::make_unique<GFSK8::Decoder>())
    {}
};

Decoder::Decoder(int submodes, int nfa, int nfb)
    : m_impl(std::make_unique<Impl>(submodes, nfa, nfb))
{}

Decoder::~Decoder() = default;

void Decoder::decode(std::span<std::int16_t const> samples,
                     int nutc,
                     DecodeCallback const &cb)
{
    // Copy supplied audio into the global dec_data buffer.
    std::size_t const n = std::min(samples.size(),
                                   static_cast<std::size_t>(GFSK8_RX_SAMPLE_SIZE));
    std::memcpy(dec_data.d2, samples.data(), n * sizeof(std::int16_t));
    if (n < static_cast<std::size_t>(GFSK8_RX_SAMPLE_SIZE))
        std::memset(dec_data.d2 + n, 0,
                    (GFSK8_RX_SAMPLE_SIZE - static_cast<int>(n)) * sizeof(std::int16_t));

    dec_data.params.nutc       = nutc;
    dec_data.params.newdat     = true;
    dec_data.params.nfa        = m_impl->nfa;
    dec_data.params.nfb        = m_impl->nfb;
    dec_data.params.nsubmodes  = m_impl->submodes;
    dec_data.params.syncStats  = false;
    dec_data.params.kin        = static_cast<int>(n);
    dec_data.params.nfqso      = 0;

    // Set each submode's decode window to cover the most recently completed
    // UTC-aligned period.  Each JS8 submode transmits on a strict UTC grid:
    // Normal every 15 s, Fast every 10 s, Turbo every 6 s, Slow every 30 s,
    // Ultra every 4 s.  Without alignment, the window for a non-Normal submode
    // would lag by up to one full period, putting most of the signal outside
    // the decoder's timing-search range and causing missed decodes.
    //
    // Algorithm: given the current UTC second-within-the-minute,
    //   elapsed = sec_in_minute % period_sec
    //   kpos    = buffer_end - (elapsed + period_sec) * sample_rate
    // This places the window start at the beginning of the last complete period.
    {
        auto const epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int const sec_in_minute = static_cast<int>(epoch_sec % 60);

        auto alignedKpos = [&](int sm) -> int {
            int const period_sec = static_cast<int>(GFSK8::Submode::period(sm));
            int const elapsed    = sec_in_minute % period_sec;
            int const kpos       = GFSK8_RX_SAMPLE_SIZE - (elapsed + period_sec) * GFSK8_RX_SAMPLE_RATE;
            return std::max(0, kpos);
        };

        int const nmaxA = static_cast<int>(GFSK8::Submode::samplesPerPeriod(0));
        int const nmaxB = static_cast<int>(GFSK8::Submode::samplesPerPeriod(1));
        int const nmaxC = static_cast<int>(GFSK8::Submode::samplesPerPeriod(2));
        int const nmaxE = static_cast<int>(GFSK8::Submode::samplesPerPeriod(4));
        int const nmaxI = static_cast<int>(GFSK8::Submode::samplesPerPeriod(8));

        dec_data.params.kposA = alignedKpos(0);
        dec_data.params.kszA  = nmaxA;
        dec_data.params.kposB = alignedKpos(1);
        dec_data.params.kszB  = nmaxB;
        dec_data.params.kposC = alignedKpos(2);
        dec_data.params.kszC  = nmaxC;
        dec_data.params.kposE = alignedKpos(4);
        dec_data.params.kszE  = nmaxE;
        dec_data.params.kposI = alignedKpos(8);
        dec_data.params.kszI  = nmaxI;
    }

    // snapshot() latches the global dec_data into the decoder's private buffer.
    m_impl->core->snapshot();

    m_impl->core->decode([&cb](GFSK8::Event::Variant const &ev) {
        if (auto const *d = std::get_if<GFSK8::Event::Decoded>(&ev)) {
            gfsk8::Decoded out;
            out.message     = d->data;
            out.snrDb       = d->snr;
            out.frequencyHz = d->frequency;
            out.dtSeconds   = d->xdt;
            out.submode     = d->mode;
            out.quality     = d->quality;
            out.frameType   = d->type;
            out.utc         = d->utc;
            cb(out);
        }
    });
}

void Decoder::reset() {
    m_impl->core = std::make_unique<GFSK8::Decoder>();
}

} // namespace gfsk8
