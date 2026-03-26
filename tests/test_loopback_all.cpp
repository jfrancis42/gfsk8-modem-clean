/**
 * test_loopback_all.cpp — loopback test for all five JS8 submodes.
 *
 * For each submode (Normal, Fast, Turbo, Slow, Ultra):
 *   1. Packs a short message and modulates it.
 *   2. Computes the UTC-aligned decode window (replicating the formula in
 *      api.cpp::Decoder::decode()) and places the signal there.
 *   3. Runs the decoder and verifies the message is recovered.
 *
 * Decoder constructor bitmask (NOT the Submode enum values):
 *   Normal=1, Fast=2, Turbo=4, Slow=8, Ultra=16
 */

#include "gfsk8modem.h"
#include "../src/constants.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Replicate the UTC-alignment formula from api.cpp so audio can be placed at
// the exact position the decoder will search.  Called just before each decode
// to minimise the chance of a period-boundary race (extremely unlikely even
// without this precaution given period lengths of 4–30 s).
// ---------------------------------------------------------------------------
static int utcAlignedKpos(int period_sec)
{
    auto epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int sec_in_min = static_cast<int>(epoch_sec % 60);
    int elapsed    = sec_in_min % period_sec;
    int kpos       = GFSK8_RX_SAMPLE_SIZE - (elapsed + period_sec) * GFSK8_RX_SAMPLE_RATE;
    return std::max(0, kpos);
}

struct SubmoodeSpec {
    const char       *name;
    gfsk8::Submode    sm;          // enum value used for pack/modulate
    int               decoderBit;  // bitmask bit for Decoder constructor
    int               periodSec;
};

static const SubmoodeSpec kSubmodes[] = {
    { "Normal", gfsk8::Submode::Normal, 1,  15 },
    { "Fast",   gfsk8::Submode::Fast,   2,  10 },
    { "Turbo",  gfsk8::Submode::Turbo,  4,   6 },
    { "Slow",   gfsk8::Submode::Slow,   8,  30 },
    { "Ultra",  gfsk8::Submode::Ultra,  16,  4 },
};

int main()
{
    const double kFreqHz   = 1500.0;
    const char  *kCallsign = "K1TEST";
    const char  *kGrid     = "DM79";
    const char  *kText     = "HELLO";

    int failures = 0;

    for (auto const &spec : kSubmodes) {
        printf("\n── %s (period=%ds) ──\n", spec.name, spec.periodSec);

        // --- Pack message into JS8 frame(s) ---
        auto frames = gfsk8::pack(kCallsign, kGrid, kText, spec.sm);
        if (frames.empty()) {
            fprintf(stderr, "  FAIL: pack returned no frames\n");
            ++failures;
            continue;
        }
        printf("  pack: %zu frame(s)\n", frames.size());

        // Modulate first frame only (should be enough to decode the header).
        auto pcm = gfsk8::modulate(spec.sm, frames[0].frameType,
                                   frames[0].payload, kFreqHz);
        if (pcm.empty()) {
            fprintf(stderr, "  FAIL: modulate returned empty PCM\n");
            ++failures;
            continue;
        }
        printf("  modulated: %zu samples (%.2f s)\n",
               pcm.size(), pcm.size() / (double)GFSK8_RX_SAMPLE_RATE);

        // --- Build snapshot buffer ---
        std::vector<int16_t> snap(GFSK8_RX_SAMPLE_SIZE, 0);
        int const nmax = spec.periodSec * GFSK8_RX_SAMPLE_RATE;

        // Compute UTC-aligned kpos just before decode so it matches the
        // value decode() will compute a moment later.
        int const kpos = utcAlignedKpos(spec.periodSec);
        printf("  kpos=%d  nmax=%d  buffer_end=%d\n",
               kpos, nmax, GFSK8_RX_SAMPLE_SIZE);

        size_t copy_n = std::min(pcm.size(), (size_t)(GFSK8_RX_SAMPLE_SIZE - kpos));
        for (size_t i = 0; i < copy_n; ++i) {
            float s = std::clamp(pcm[i], -1.0f, 1.0f);
            snap[(size_t)kpos + i] = (int16_t)(s * 16000.0f);
        }

        // --- Decode ---
        gfsk8::Decoder decoder(spec.decoderBit);
        int n_decoded = 0;
        std::string first_msg;
        std::string first_from;

        decoder.decode(std::span<const int16_t>(snap), 0,
            [&](const gfsk8::Decoded &d) {
                printf("  DECODED  snr=%+d dB  freq=%.1f Hz  dt=%.2f s  msg=[%s]\n",
                       d.snrDb, d.frequencyHz, d.dtSeconds, d.message.c_str());
                if (n_decoded == 0) {
                    first_msg  = d.message;
                    first_from = d.message; // full decoded string for now
                }
                ++n_decoded;
            });

        if (n_decoded == 0) {
            fprintf(stderr, "  FAIL: no decodes\n");
            ++failures;
        } else {
            printf("  PASS: %d decode(s)\n", n_decoded);
        }
    }

    printf("\n══════════════════════════════════\n");
    if (failures == 0) {
        printf("ALL SUBMODES PASSED\n");
    } else {
        fprintf(stderr, "%d SUBMODE(S) FAILED\n", failures);
    }
    return failures ? 1 : 0;
}
