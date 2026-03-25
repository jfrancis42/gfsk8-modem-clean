/**
 * @file JS8Submode.cpp
 * @brief JS8 submode parameter table and inquiry functions.
 *
 * Rewritten from the original JS8Submode.cpp.  The public interface is
 * identical (same function signatures and return values) but the internal
 * implementation uses:
 *   - A struct named SubmodeDef instead of Data.
 *   - Member variables with full descriptive names instead of m_-prefixed
 *     abbreviations.
 *   - A free function `lookup()` (instead of `data()`) that returns a
 *     const reference to the appropriate SubmodeDef.
 *   - A constexpr integer floor helper renamed `ifloor` to avoid shadowing
 *     std::floor.
 */
#include "JS8Submode.h"
#include "commons.h"
#include "log.h"

#include <concepts>

namespace GFSK8::Submode {
namespace {

// ── Compile-time floor for positive integers ──────────────────────────────────
// std::floor is not constexpr until C++23; provide a simple integer version.

template <std::floating_point T>
constexpr int ifloor(T v)
{
    auto i = static_cast<int>(v);
    return (v < static_cast<T>(i)) ? i - 1 : i;
}

static_assert(ifloor(0.0)     ==   0);
static_assert(ifloor(0.5)     ==   0);
static_assert(ifloor(0.9999)  ==   0);
static_assert(ifloor(1.0)     ==   1);
static_assert(ifloor(123.4)   == 123);
static_assert(ifloor(-0.5)    ==  -1);
static_assert(ifloor(-1.0)    ==  -1);
static_assert(ifloor(-123.4)  == -124);

// ── Per-submode compile-time data ─────────────────────────────────────────────

/// Holds all constant parameters for one JS8 submode.  All derived fields
/// are computed in the constructor so callers can rely on them directly.
struct SubmodeDef {
    const char   *modeName;
    unsigned int  spss;          // samples per symbol (raw input)
    unsigned int  txPeriodSecs;
    unsigned int  startDelaySamples; // ms → samples done at access time
    unsigned int  startDelayMs;
    Costas::Type  costasType;
    int           snrThresholdDb;
    int           decodeThreshold;

    // Derived fields (all computed in the constructor).
    int          totalSymbolSamples;  // GFSK8_NUM_SYMBOLS * spss
    int          signalBandwidthHz;   // 8 * GFSK8_RX_SAMPLE_RATE / spss
    int          samplesInPeriod;     // GFSK8_RX_SAMPLE_RATE * txPeriodSecs
    double       toneSpacingHz;       // GFSK8_RX_SAMPLE_RATE / spss
    int          samplesRequired;     // floor(totalSymbolSamples + (0.5 + delayMs/1000)*SR)
    double       dataDurationSecs;    // totalSymbolSamples / SR
    double       txDurationSecs;      // dataDurationSecs + delayMs/1000

    constexpr SubmodeDef(const char *name,
                         unsigned int samplesPerSymbol,
                         unsigned int startDelay_ms,
                         unsigned int period_secs,
                         Costas::Type ctype,
                         int snrThresh,
                         int decThresh = 10)
        : modeName(name),
          spss(samplesPerSymbol),
          txPeriodSecs(period_secs),
          startDelaySamples(startDelay_ms * GFSK8_RX_SAMPLE_RATE / 1000),
          startDelayMs(startDelay_ms),
          costasType(ctype),
          snrThresholdDb(snrThresh),
          decodeThreshold(decThresh),
          totalSymbolSamples(GFSK8_NUM_SYMBOLS * static_cast<int>(samplesPerSymbol)),
          signalBandwidthHz(8 * GFSK8_RX_SAMPLE_RATE / static_cast<int>(samplesPerSymbol)),
          samplesInPeriod(GFSK8_RX_SAMPLE_RATE * static_cast<int>(period_secs)),
          toneSpacingHz(static_cast<double>(GFSK8_RX_SAMPLE_RATE) / samplesPerSymbol),
          samplesRequired(ifloor(
              static_cast<double>(GFSK8_NUM_SYMBOLS * samplesPerSymbol) +
              (0.5 + startDelay_ms / 1000.0) * GFSK8_RX_SAMPLE_RATE)),
          dataDurationSecs(static_cast<double>(GFSK8_NUM_SYMBOLS * samplesPerSymbol)
                           / GFSK8_RX_SAMPLE_RATE),
          txDurationSecs(static_cast<double>(GFSK8_NUM_SYMBOLS * samplesPerSymbol)
                         / GFSK8_RX_SAMPLE_RATE
                         + startDelay_ms / 1000.0)
    {}
};

// ── Submode instances ─────────────────────────────────────────────────────────

constexpr SubmodeDef kNormal{ "NORMAL",  GFSK8_NORMAL_SYMBOL_SAMPLES, GFSK8_NORMAL_START_DELAY_MS,
                               GFSK8_NORMAL_TX_SECONDS, Costas::Type::ORIGINAL, -24 };

constexpr SubmodeDef kFast{   "FAST",    GFSK8_FAST_SYMBOL_SAMPLES, GFSK8_FAST_START_DELAY_MS,
                               GFSK8_FAST_TX_SECONDS, Costas::Type::MODIFIED, -22, 16 };

constexpr SubmodeDef kTurbo{  "JS8 40",  GFSK8_TURBO_SYMBOL_SAMPLES, GFSK8_TURBO_START_DELAY_MS,
                               GFSK8_TURBO_TX_SECONDS, Costas::Type::MODIFIED, -20, 32 };

constexpr SubmodeDef kSlow{   "SLOW",    GFSK8_SLOW_SYMBOL_SAMPLES, GFSK8_SLOW_START_DELAY_MS,
                               GFSK8_SLOW_TX_SECONDS, Costas::Type::MODIFIED, -28 };

constexpr SubmodeDef kUltra{  "JS8 60",  GFSK8_ULTRA_SYMBOL_SAMPLES, GFSK8_ULTRA_START_DELAY_MS,
                               GFSK8_ULTRA_TX_SECONDS, Costas::Type::MODIFIED, -18, 50 };

// ── Dispatch ──────────────────────────────────────────────────────────────────

/// Return the SubmodeDef for the given submode integer, or throw.
constexpr SubmodeDef const &lookup(int sm)
{
    switch (sm) {
    case 0: return kNormal;
    case 1: return kFast;
    case 2: return kTurbo;
    case 4: return kSlow;
    case 8: return kUltra;
    default:
        throw error{ "Invalid JS8 submode " + std::to_string(sm) };
    }
}

} // anonymous namespace

// ── Public accessors ──────────────────────────────────────────────────────────

std::string  name(int sm)              { return lookup(sm).modeName; }
unsigned int bandwidth(int sm)         { return static_cast<unsigned>(lookup(sm).signalBandwidthHz); }
Costas::Type costas(int sm)            { return lookup(sm).costasType; }
unsigned int period(int sm)            { return lookup(sm).txPeriodSecs; }
unsigned int samplesForOneSymbol(int sm) { return lookup(sm).spss; }
unsigned int samplesForSymbols(int sm) { return static_cast<unsigned>(lookup(sm).totalSymbolSamples); }
unsigned int samplesNeeded(int sm)     { return static_cast<unsigned>(lookup(sm).samplesRequired); }
unsigned int samplesPerPeriod(int sm)  { return static_cast<unsigned>(lookup(sm).samplesInPeriod); }
int          rxSNRThreshold(int sm)    { return lookup(sm).snrThresholdDb; }
int          rxThreshold(int sm)       { return lookup(sm).decodeThreshold; }
unsigned int startDelayMS(int sm)      { return lookup(sm).startDelayMs; }
double       toneSpacing(int sm)       { return lookup(sm).toneSpacingHz; }
double       dataDuration(int sm)      { return lookup(sm).dataDurationSecs; }
double       txDuration(int sm)        { return lookup(sm).txDurationSecs; }

// ── Cycle helpers ─────────────────────────────────────────────────────────────

int computeCycleForDecode(int const sm, int const k)
{
    int const ringSize    = GFSK8_RX_SAMPLE_SIZE;
    int const periodSize  = static_cast<int>(samplesPerPeriod(sm));
    return (k / periodSize) % (ringSize / periodSize);
}

int computeAltCycleForDecode(int const sm, int const k, int const offsetFrames)
{
    int const altK = k - offsetFrames;
    return computeCycleForDecode(sm, altK < 0 ? altK + GFSK8_RX_SAMPLE_SIZE : altK);
}

double computeRatio(int const sm, double const p)
{
    return (p - lookup(sm).dataDurationSecs) / p;
}

} // namespace GFSK8::Submode
