/**
 * @file test_encode.cpp
 * @brief Smoke test: encode a 12-character JS8 payload and verify tone count.
 *
 * This test does NOT require a reference JS8Call binary; it only checks that
 * encode() returns the expected number of tones in the expected range.
 */

#include "gfsk8modem.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    // A valid 12-character JS8 physical-layer payload.
    // The 6-bit alphabet is: 0-9, A-Z, a-z, +, -  (64 symbols; NO space).
    // Human-readable messages are packed into this alphabet by the message
    // layer (Varicode), which is NOT part of this library.
    std::string const msg = "HELLOWORLD--";

    for (auto submode : {gfsk8::Submode::Normal, gfsk8::Submode::Fast,
                         gfsk8::Submode::Turbo, gfsk8::Submode::Slow,
                         gfsk8::Submode::Ultra})
    {
        std::vector<int> tones;
        bool ok = gfsk8::encode(submode, 0, msg, tones);

        assert(ok && "encode() returned false");
        assert(tones.size() == 79 && "expected 79 tones");

        for (int t : tones) {
            assert(t >= 0 && t <= 7 && "tone out of range [0,7]");
        }

        gfsk8::SubmodeParms p = gfsk8::submodeParms(submode);
        assert(p.numSymbols == 79);
        assert(p.sampleRate == 12000);

        std::printf("Submode %d: OK  sps=%d  spacing=%.3f Hz  period=%ds\n",
                    static_cast<int>(submode),
                    p.samplesPerSymbol, p.toneSpacingHz, p.periodSeconds);
    }

    // Verify modulate() returns non-empty audio for Normal mode
    auto pcm = gfsk8::modulate(gfsk8::Submode::Normal, 0, msg);
    assert(!pcm.empty() && "modulate() returned empty vector");

    gfsk8::SubmodeParms p = gfsk8::submodeParms(gfsk8::Submode::Normal);
    int const expected = p.startDelayMs * 12000 / 1000 + 79 * p.samplesPerSymbol;
    assert(static_cast<int>(pcm.size()) == expected && "unexpected PCM length");

    std::printf("modulate(Normal): %zu samples\n", pcm.size());
    std::puts("All encode tests PASSED.");
    return 0;
}
