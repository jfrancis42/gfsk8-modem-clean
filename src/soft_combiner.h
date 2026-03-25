/**
 * @file soft_combiner.h
 * @brief LLR soft-combining cache for the JS8 decoder.
 *
 * Accumulates repeated LLR frames from the same candidate signal to improve
 * decode probability.  Candidates are keyed by a coarse (mode, frequency-bin,
 * dt-bin) bucket plus a 32-bit LLR signature; repeated frames in the same
 * bucket are vector-added to the stored LLRs.
 *
 * Rewritten from the original soft_combiner.h with the following differences:
 *   - Bucket / Entry / CoarseKey renamed to LlrBucket / LlrEntry / BinKey.
 *   - Tag member of Entry renamed from `lastSeen` to `timestamp`.
 *   - `defaultEnabled()` factored into the constructor directly.
 *   - Hamming-distance helper moved to a named free function in the
 *     local detail namespace.
 */
#pragma once

#include "log.h"
#include <cstdlib>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gfsk8 {

namespace detail {

/// Hamming distance between two 32-bit values.
inline int hammingDistance(uint32_t a, uint32_t b) noexcept
{
    uint32_t diff = a ^ b;
    int count = 0;
    while (diff) { diff &= diff - 1; ++count; }
    return count;
}

} // namespace detail

/**
 * @brief Cache and combine repeated LLR frames for the same decode candidate.
 *
 * Templated on N = number of LLR values per frame (bound to the LDPC bit count
 * at instantiation).
 */
template <std::size_t N>
class SoftCombiner {
    using Clock = std::chrono::steady_clock;

public:
    // ── Public types ─────────────────────────────────────────────────────────

    struct Key {
        int      mode;
        int      freqBin;
        int      dtBin;
        uint32_t signature;

        bool operator==(Key const &o) const noexcept {
            return mode == o.mode && freqBin == o.freqBin &&
                   dtBin == o.dtBin && signature == o.signature;
        }
    };

    struct Combined {
        Key                   key;
        std::array<float, N>  llr0;
        std::array<float, N>  llr1;
        int                   repeats;
        bool                  wasCombined;
    };

    // ── Construction ─────────────────────────────────────────────────────────

    SoftCombiner() : SoftCombiner(readDefaultEnabled(), true) {}

    explicit SoftCombiner(bool enabled, bool runSelfTest = true)
        : m_enabled(enabled)
    {
        if (runSelfTest) selfTest();
    }

    // ── Key construction ─────────────────────────────────────────────────────

    Key makeKey(int mode, float freq, float dt,
                std::array<float, N> const &llr0,
                std::array<float, N> const &llr1) const
    {
        return Key{ mode,
                    static_cast<int>(std::lround(freq)),
                    static_cast<int>(std::lround(dt * 10.0f)), // 100 ms bins
                    computeSignature(llr0, llr1) };
    }

    // ── Combine ──────────────────────────────────────────────────────────────

    Combined combine(Key const &key,
                     std::array<float, N> const &llr0,
                     std::array<float, N> const &llr1,
                     std::chrono::seconds ttl)
    {
        flush(ttl);

        if (!m_enabled)
            return Combined{ key, llr0, llr1, 1, false };

        auto &bucket = m_store[binKeyFor(key)];
        auto  it     = findEntry(bucket, key.signature);

        if (it == bucket.end()) {
            bucket.push_back(makeEntry(key.signature, llr0, llr1));
            return Combined{ key, llr0, llr1, 1, false };
        }

        for (std::size_t i = 0; i < N; ++i) {
            it->llr0[i] += llr0[i];
            it->llr1[i] += llr1[i];
        }
        ++it->repeats;
        it->timestamp = Clock::now();

        qCDebug(decoder_js8) << "soft-combining repeats" << it->repeats
                             << "mode" << key.mode
                             << "freq" << key.freqBin
                             << "dtbin" << key.dtBin;

        return Combined{ key, it->llr0, it->llr1, it->repeats, true };
    }

    // ── Post-decode cleanup ──────────────────────────────────────────────────

    void markDecoded(Key const &key)
    {
        if (!m_enabled) return;
        auto it = m_store.find(binKeyFor(key));
        if (it == m_store.end()) return;

        auto &bucket = it->second;
        bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                    [&](LlrEntry const &e) {
                                        return e.signature == key.signature;
                                    }),
                     bucket.end());
        if (bucket.empty()) m_store.erase(it);
    }

    void flush(std::chrono::seconds ttl)
    {
        if (!m_enabled) return;
        auto const now = Clock::now();

        for (auto it = m_store.begin(); it != m_store.end(); ) {
            auto &bucket = it->second;
            bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
                                        [&](LlrEntry const &e) {
                                            return now - e.timestamp > ttl;
                                        }),
                         bucket.end());
            it = bucket.empty() ? m_store.erase(it) : std::next(it);
        }
    }

private:
    // ── Private types ─────────────────────────────────────────────────────────

    struct BinKey {
        int mode, freqBin, dtBin;
        bool operator==(BinKey const &o) const noexcept {
            return mode == o.mode && freqBin == o.freqBin && dtBin == o.dtBin;
        }
    };

    struct BinKeyHash {
        std::size_t operator()(BinKey const &k) const noexcept {
            std::size_t h1 = std::hash<int>{}(k.mode);
            std::size_t h2 = std::hash<int>{}(k.freqBin);
            std::size_t h3 = std::hash<int>{}(k.dtBin);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    struct LlrEntry {
        uint32_t             signature;
        std::array<float, N> llr0;
        std::array<float, N> llr1;
        int                  repeats;
        Clock::time_point    timestamp;
    };

    using LlrBucket = std::vector<LlrEntry>;

    // ── Signature computation ─────────────────────────────────────────────────

    static constexpr std::array<int, 32> buildSigIndices()
    {
        std::array<int, 32> idx{};
        int v = 0;
        for (std::size_t i = 0; i < idx.size(); ++i) {
            v = (v + 37) % static_cast<int>(N);
            idx[i] = v;
        }
        return idx;
    }

    static uint32_t computeSignature(std::array<float, N> const &llr0,
                                     std::array<float, N> const &llr1)
    {
        static constexpr auto INDICES = buildSigIndices();
        uint32_t sig = 0;
        for (std::size_t i = 0; i < INDICES.size(); ++i) {
            float v = 0.5f * (llr0[INDICES[i]] + llr1[INDICES[i]]);
            if (v >= 0.0f) sig |= (1u << i);
        }
        return sig;
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    BinKey binKeyFor(Key const &k) const { return BinKey{ k.mode, k.freqBin, k.dtBin }; }

    static typename LlrBucket::iterator findEntry(LlrBucket &bucket, uint32_t sig)
    {
        constexpr int MAX_HAMMING = 4;
        return std::find_if(bucket.begin(), bucket.end(),
                            [sig](LlrEntry const &e) {
                                return detail::hammingDistance(sig, e.signature) <= MAX_HAMMING;
                            });
    }

    static LlrEntry makeEntry(uint32_t sig,
                              std::array<float, N> const &llr0,
                              std::array<float, N> const &llr1)
    {
        return LlrEntry{ sig, llr0, llr1, 1, Clock::now() };
    }

    static bool readDefaultEnabled()
    {
        const char *e = std::getenv("GFSK8_SOFT_COMBINING");
        return e ? (std::atoi(e) != 0) : true;
    }

    // ── Self-test (opt-in via GFSK8_SOFT_COMBINING_TEST) ────────────────────────

    static void selfTest()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            if (!std::getenv("GFSK8_SOFT_COMBINING_TEST")) return;

            SoftCombiner sc(true, false);
            std::array<float, N> baseline{};
            for (std::size_t i = 0; i < N; ++i)
                baseline[i] = (i % 2 == 0) ? 2.0f : -2.0f;

            auto perturb = [](std::array<float, N> b, int stride) {
                for (std::size_t i = 0; i < b.size(); ++i)
                    b[i] *= (i % stride == 0) ? -0.4f : 0.8f;
                return b;
            };

            auto a = perturb(baseline, 7);
            auto b = perturb(baseline, 11);
            auto key = sc.makeKey(0, 1500.0f, 1.0f, a, b);
            sc.combine(key, a, a, std::chrono::seconds{30});
            auto result = sc.combine(key, b, b, std::chrono::seconds{30});

            auto countSign = [&](std::array<float, N> const &llr) {
                int m = 0;
                for (std::size_t i = 0; i < N; ++i)
                    if (llr[i] * baseline[i] > 0.0f) ++m;
                return m;
            };
            qCDebug(decoder_js8) << "soft-combining self-test:"
                                 << "combined matches" << countSign(result.llr0)
                                 << "repeats" << result.repeats;
        });
    }

    // ── Data members ─────────────────────────────────────────────────────────

    std::unordered_map<BinKey, LlrBucket, BinKeyHash> m_store;
    bool m_enabled;
};

} // namespace gfsk8
