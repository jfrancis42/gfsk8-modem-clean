#pragma once
/**
 * @file whitening.h
 * @brief Per-tone / per-symbol noise whitening and LLR computation for JS8.
 *
 * Given the 8×58 symbol-magnitude matrix (sans Costas pilots) and the index
 * of the winning tone for each symbol, this processor:
 *   1. Estimates per-tone noise medians from non-winning magnitudes.
 *   2. Estimates per-symbol noise medians.
 *   3. Computes two sets of log-likelihood ratios (LLR0 using linear
 *      magnitudes; LLR1 using log magnitudes).
 *   4. Optionally applies noise whitening and erasure thresholding.
 *   5. Normalises both LLR arrays to unit variance × 2.83.
 *
 * The template parameters NROWS (8), ND (58), and N (174) are bound at
 * compile time so the class is completely header-only.
 */

#include "log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <vector>

namespace gfsk8 {

template <int NROWS, int ND, int N>
class WhiteningProcessor {
public:
    struct Result {
        std::array<float, 3 * ND> llr0;        // linear-magnitude LLRs
        std::array<float, 3 * ND> llr1;        // log-magnitude LLRs
        bool   whiteningApplied;
        bool   erasureApplied;
        std::size_t erasures;
        double avgAbsPre;
        double avgAbsPost;
    };

    static Result process(
        std::array<std::array<float, ND>, NROWS> const &s1,
        std::array<int, ND> const &symbolWinners,
        float erasureThreshold,
        bool  debug)
    {
        // Compute median of a mutable vector.
        auto const medianOf = [](std::vector<float> &v) -> std::optional<float> {
            if (v.empty()) return std::nullopt;
            auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
            std::nth_element(v.begin(), mid, v.end());
            float m = *mid;
            if (v.size() % 2 == 0 && v.size() > 1) {
                std::nth_element(v.begin(),
                                 v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2 - 1),
                                 v.end());
                m = 0.5f * (m + v[v.size() / 2 - 1]);
            }
            return m;
        };

        // Per-tone noise: median of non-winning magnitudes across all symbols.
        auto const toneNoise = [&]() -> std::optional<std::array<float, NROWS>> {
            std::array<std::vector<float>, NROWS> samples;
            for (int j = 0; j < ND; ++j) {
                int const w = symbolWinners[j];
                for (int i = 0; i < NROWS; ++i) {
                    if (i != w) samples[i].push_back(s1[i][j]);
                }
            }
            std::array<float, NROWS> noise{};
            for (int i = 0; i < NROWS; ++i) {
                auto m = medianOf(samples[i]);
                if (!m) return std::nullopt;
                noise[i] = *m;
            }
            return noise;
        }();

        // Per-symbol noise: median of non-winning magnitudes for each symbol.
        auto const symbolNoise = [&]() -> std::optional<std::vector<float>> {
            std::vector<float> noise;
            noise.reserve(ND);
            for (int j = 0; j < ND; ++j) {
                std::vector<float> bins;
                bins.reserve(NROWS - 1);
                int const w = symbolWinners[j];
                for (int i = 0; i < NROWS; ++i) {
                    if (i != w) bins.push_back(s1[i][j]);
                }
                auto m = medianOf(bins);
                if (!m) return std::nullopt;
                noise.push_back(*m);
            }
            return noise;
        }();

        bool const disableWhitening = (std::getenv("GFSK8_DISABLE_WHITENING") != nullptr);
        bool const canWhiten = toneNoise && symbolNoise &&
                               !symbolNoise->empty() && !disableWhitening;
        bool const doErasure = canWhiten && erasureThreshold > 0.0f;

        Result result{};
        double sumPre = 0.0, sumPost = 0.0;
        std::size_t erasures = 0;

        for (int j = 0; j < ND; ++j) {
            int const i1 = 3 * j;
            int const i2 = 3 * j + 1;
            int const i4 = 3 * j + 2;

            // Load per-column magnitudes.
            std::array<float, NROWS> ps;
            for (int i = 0; i < NROWS; ++i) ps[i] = s1[i][j];

            // LLR0: linear-magnitude, 3 bits of 8-FSK demapping.
            // r4 bit: high half (4-7) vs low half (0-3)
            // r2 bit: (2,3,6,7) vs (0,1,4,5)
            // r1 bit: odd (1,3,5,7) vs even (0,2,4,6)
            result.llr0[i1] = std::max({ps[4],ps[5],ps[6],ps[7]}) - std::max({ps[0],ps[1],ps[2],ps[3]});
            result.llr0[i2] = std::max({ps[2],ps[3],ps[6],ps[7]}) - std::max({ps[0],ps[1],ps[4],ps[5]});
            result.llr0[i4] = std::max({ps[1],ps[3],ps[5],ps[7]}) - std::max({ps[0],ps[2],ps[4],ps[6]});

            // LLR1: log-magnitude version.
            for (auto &x : ps) x = std::log(x + 1e-32f);
            result.llr1[i1] = std::max({ps[4],ps[5],ps[6],ps[7]}) - std::max({ps[0],ps[1],ps[2],ps[3]});
            result.llr1[i2] = std::max({ps[2],ps[3],ps[6],ps[7]}) - std::max({ps[0],ps[1],ps[4],ps[5]});
            result.llr1[i4] = std::max({ps[1],ps[3],ps[5],ps[7]}) - std::max({ps[0],ps[2],ps[4],ps[6]});

            if (canWhiten) {
                int const w = symbolWinners[j];
                float const tn = std::max(0.0f, (*toneNoise)[w]);
                float const sn = std::max(0.0f, (*symbolNoise)[j]);
                float const localNoise = std::sqrt(tn * sn + 1e-12f);

                auto applyWhitening = [&](float &v) {
                    sumPre += std::abs(v);
                    if (localNoise > 0.0f && std::isfinite(localNoise))
                        v /= localNoise;
                    if (doErasure && std::abs(v) < erasureThreshold) {
                        v = 0.0f;
                        ++erasures;
                    }
                    sumPost += std::abs(v);
                };

                applyWhitening(result.llr0[i1]);
                applyWhitening(result.llr0[i2]);
                applyWhitening(result.llr0[i4]);
                applyWhitening(result.llr1[i1]);
                applyWhitening(result.llr1[i2]);
                applyWhitening(result.llr1[i4]);
            }
        }

        // Normalise LLRs to zero mean and std ≈ 2.83.
        auto normalizeLLR = [](auto &llr) {
            float sum = 0.0f, sum2 = 0.0f;
            for (auto v : llr) { sum += v; sum2 += v * v; }
            float const avg = sum / static_cast<float>(llr.size());
            float const var = sum2 / static_cast<float>(llr.size()) - avg * avg;
            float const sig = std::sqrt(var > 0.0f ? var : sum2 / static_cast<float>(llr.size()));
            if (sig > 0.0f) {
                for (float &v : llr) v = (v / sig) * 2.83f;
            }
        };
        normalizeLLR(result.llr0);
        normalizeLLR(result.llr1);

        result.whiteningApplied = canWhiten;
        result.erasureApplied   = doErasure;
        result.erasures         = erasures;
        result.avgAbsPre        = sumPre;
        result.avgAbsPost       = sumPost;
        return result;
    }
};

} // namespace gfsk8
