/**
 * test_loopback.cpp — encode a message, modulate it, decode it.
 *
 * Tests ModeA (Normal) at a generous SNR.  Passes if the decoded
 * message matches the input.
 *
 * Audio is placed at the UTC-aligned decode window position (matching the
 * formula in api.cpp::Decoder::decode()) so the test remains valid after
 * the period-alignment fix.
 */

#include "gfsk8modem.h"
#include "../src/constants.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <random>
#include <vector>

int main()
{
    // 12-char JS8-alphabet message
    const char *msg = "TESTTEST1234";
    const double freq_hz = 1500.0;

    // --- Encode + modulate ---
    std::vector<float> pcm = gfsk8::modulate(gfsk8::Submode::Normal, 0, msg, freq_hz);
    if (pcm.empty()) { fprintf(stderr, "modulate failed\n"); return 1; }
    printf("modulated: %zu samples (%.2f s)\n", pcm.size(), pcm.size() / 12000.0);

    // --- Build 720000-sample snapshot ---
    // Place audio at the UTC-aligned kpos that decode() will compute,
    // so the signal falls exactly at the start of the search window.
    std::vector<int16_t> snap(GFSK8_RX_SAMPLE_SIZE, 0);
    int const period_sec = gfsk8::submodeParms(gfsk8::Submode::Normal).periodSeconds; // 15
    int const nmaxA      = period_sec * GFSK8_RX_SAMPLE_RATE;

    auto epoch_sec  = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int sec_in_min  = static_cast<int>(epoch_sec % 60);
    int elapsed     = sec_in_min % period_sec;
    int const kposA = std::max(0, GFSK8_RX_SAMPLE_SIZE - (elapsed + period_sec) * GFSK8_RX_SAMPLE_RATE);

    // Convert float PCM → int16, place at kposA
    size_t copy_n = std::min(pcm.size(), (size_t)(GFSK8_RX_SAMPLE_SIZE - kposA));
    for (size_t i = 0; i < copy_n; ++i) {
        float s = std::clamp(pcm[i], -1.0f, 1.0f);
        snap[(size_t)kposA + i] = (int16_t)(s * 16000.0f);
    }

    printf("audio placed at snap[%d..%d)\n", kposA, kposA + nmaxA);

    // --- Decode ---
    gfsk8::Decoder decoder(1 << 0); // Normal only
    int n_decoded = 0;
    std::string decoded_msg;

    decoder.decode(std::span<const int16_t>(snap), 0,
        [&](const gfsk8::Decoded &d) {
            printf("DECODED  snr=%+d dB  freq=%.1f Hz  dt=%.2f s  msg=[%s]\n",
                   d.snrDb, d.frequencyHz, d.dtSeconds, d.message.c_str());
            decoded_msg = d.message;
            ++n_decoded;
        });

    if (n_decoded == 0) {
        fprintf(stderr, "FAIL: no decodes\n");
        return 1;
    }
    if (decoded_msg != msg) {
        fprintf(stderr, "FAIL: decoded [%s] expected [%s]\n",
                decoded_msg.c_str(), msg);
        return 1;
    }
    printf("PASS: decoded [%s]\n", decoded_msg.c_str());
    return 0;
}
