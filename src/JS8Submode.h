/**
 * @file JS8Submode.h
 * @brief JS8 submode parameter inquiry interface.
 *
 * Each JS8 submode (Normal/Fast/Turbo/Slow/Ultra) has a fixed set of signal
 * parameters (samples per symbol, tone spacing, TX period, start delay, etc.).
 * The functions here look up those parameters by the integer submode identifier
 * that matches JS8Call-improved's Varicode::SubModeType enum values.
 *
 * Rewritten from the original JS8Submode.h; the interface is identical but
 * the implementation (in JS8Submode.cpp) uses a different internal data class.
 */
#pragma once

#include "JS8.h"

#include <stdexcept>
#include <string>

namespace GFSK8::Submode {

/// Thrown when an invalid submode identifier is passed to any function here.
struct error : public std::runtime_error {
    explicit error(std::string const &what) : std::runtime_error(what) {}
};

// ── Parameter accessors ───────────────────────────────────────────────────────
// All functions accept an integer submode identifier:
//   0 = Normal, 1 = Fast, 2 = Turbo, 4 = Slow, 8 = Ultra

std::string  name(int submode);
unsigned int bandwidth(int submode);
Costas::Type costas(int submode);
unsigned int period(int submode);
unsigned int samplesForOneSymbol(int submode);
unsigned int samplesForSymbols(int submode);
unsigned int samplesNeeded(int submode);
unsigned int samplesPerPeriod(int submode);
int          rxSNRThreshold(int submode);
int          rxThreshold(int submode);
unsigned int startDelayMS(int submode);
double       toneSpacing(int submode);
double       dataDuration(int submode);
double       txDuration(int submode);

// ── Cycle / ratio helpers ─────────────────────────────────────────────────────

int    computeCycleForDecode(int submode, int k);
int    computeAltCycleForDecode(int submode, int k, int offsetFrames);
double computeRatio(int submode, double period);

} // namespace GFSK8::Submode
