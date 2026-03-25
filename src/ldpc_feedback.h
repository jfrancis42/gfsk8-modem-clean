/**
 * @file ldpc_feedback.h
 * @brief LDPC erasure-threshold configuration and LLR-feedback refinement.
 *
 * Provides:
 *   - Run-time tunable thresholds read from environment variables.
 *   - refineLlrsWithLdpcFeedback<N>(): boosts high-confidence LLRs and
 *     shrinks uncertain ones based on a decoded codeword estimate; used
 *     between successive LDPC passes inside the JS8 decoder loop.
 *
 * Rewritten from the original ldpc_feedback.h with the following differences:
 *   - All constants renamed with an LLR_ / LDPC_ prefix replaced by a
 *     Threshold_ / Feedback_ prefix for readability.
 *   - The lambda-based env-reading idiom replaced by a named helper.
 *   - Doxygen comments added throughout.
 */
#pragma once

#include "log.h"
#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace gfsk8 {

// ── Compile-time defaults ─────────────────────────────────────────────────────

constexpr float Threshold_ErasureDefault     = 0.25f;
constexpr float Threshold_ConfidentMin       = 3.0f;
constexpr float Threshold_UncertainMax       = 1.0f;
constexpr float Feedback_ConfidentBoost      = 1.2f;
constexpr float Feedback_UncertainShrink     = 0.5f;
constexpr float Feedback_MagnitudeCap        = 6.0f;
constexpr int   Feedback_DefaultMaxPasses    = 8;

// ── Environment-variable readers ─────────────────────────────────────────────

namespace detail {

/// Read an integer env variable; return the default when absent or invalid.
inline int readEnvInt(const char *name, int fallback)
{
    const char *e = std::getenv(name);
    return e ? std::atoi(e) : fallback;
}

/// Return true when the named env variable is set (any value).
inline bool envSet(const char *name) { return std::getenv(name) != nullptr; }

} // namespace detail

/**
 * @brief Return the erasure threshold for LLR zeroing.
 *
 * Reads GFSK8_LLR_ERASURE_THRESH; falls back to Threshold_ErasureDefault.
 * Returns 0.0f when erasure thresholding is disabled via
 * GFSK8_DISABLE_ERASURE_THRESHOLDING or when the threshold is non-positive.
 */
inline float llrErasureThreshold()
{
    float thr = Threshold_ErasureDefault;

    if (const char *e = std::getenv("GFSK8_LLR_ERASURE_THRESH"); e) {
        char *end = nullptr;
        float v = std::strtof(e, &end);
        if (end != e && std::isfinite(v))
            thr = v;
    }

    if (thr <= 0.0f || !std::isfinite(thr) ||
        detail::envSet("GFSK8_DISABLE_ERASURE_THRESHOLDING"))
        return 0.0f;

    return thr;
}

/**
 * @brief Return true if LDPC feedback refinement is enabled.
 *
 * Reads GFSK8_LDPC_FEEDBACK; defaults to enabled when absent.
 */
inline bool ldpcFeedbackEnabled()
{
    const char *e = std::getenv("GFSK8_LDPC_FEEDBACK");
    return e ? (std::atoi(e) != 0) : true;
}

/**
 * @brief Return the maximum number of LDPC feedback passes.
 *
 * Reads GFSK8_LDPC_MAX_PASSES; clamps to [1, Feedback_DefaultMaxPasses].
 */
inline int ldpcFeedbackMaxPasses()
{
    const char *e = std::getenv("GFSK8_LDPC_MAX_PASSES");
    if (!e) return Feedback_DefaultMaxPasses;
    return std::clamp(std::atoi(e), 1, Feedback_DefaultMaxPasses);
}

// ── LLR refinement ────────────────────────────────────────────────────────────

/**
 * @brief Refine an LLR array using a decoded codeword estimate.
 *
 * For each bit position:
 *   - If the decoded bit agrees with the LLR sign and |LLR| is large
 *     (>= Threshold_ConfidentMin): boost magnitude by Feedback_ConfidentBoost,
 *     capped at Feedback_MagnitudeCap.
 *   - If the decoded bit disagrees with the LLR sign, or |LLR| is small
 *     (<= Threshold_UncertainMax): shrink by Feedback_UncertainShrink;
 *     optionally zero if shrunk below erasureThreshold.
 *
 * @param llrIn           Input LLR array.
 * @param cw              Decoded codeword (0 = bit-0, non-zero = bit-1).
 * @param erasureThreshold If > 0, values below this are zeroed after shrinking.
 * @param llrOut          Output refined LLR array.
 * @param confidentCount  Out: number of bits whose magnitude was boosted.
 * @param uncertainCount  Out: number of bits whose magnitude was shrunk.
 */
template <std::size_t N>
void refineLlrsWithLdpcFeedback(std::array<float, N> const &llrIn,
                                std::array<int8_t, N> const &cw,
                                float erasureThreshold,
                                std::array<float, N> &llrOut,
                                int &confidentCount, int &uncertainCount)
{
    llrOut = llrIn;
    confidentCount = uncertainCount = 0;

    for (std::size_t i = 0; i < llrOut.size(); ++i) {
        float &v = llrOut[i];

        if (!std::isfinite(v)) {
            v = 0.0f;
            ++uncertainCount;
            continue;
        }

        bool const bitIsOne   = (cw[i] != 0);
        float const magnitude = std::abs(v);
        bool const signAgrees = (v >= 0.0f) == bitIsOne;

        if (signAgrees && magnitude >= Threshold_ConfidentMin) {
            ++confidentCount;
            float boosted = std::clamp(magnitude * Feedback_ConfidentBoost,
                                       0.0f, Feedback_MagnitudeCap);
            v = bitIsOne ? boosted : -boosted;
        } else if (!signAgrees || magnitude <= Threshold_UncertainMax) {
            ++uncertainCount;
            float shrunk = magnitude * Feedback_UncertainShrink;
            if (erasureThreshold > 0.0f && shrunk < erasureThreshold)
                v = 0.0f;
            else
                v = bitIsOne ? shrunk : -shrunk;
        }
    }
}

} // namespace gfsk8
