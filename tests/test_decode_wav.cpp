/**
 * test_decode_wav.cpp — decode a 12kHz 16-bit mono WAV file through gfsk8modem.
 *
 * Usage:  test_decode_wav <file.wav> [submode]
 *   submode: 0=Normal(default), 1=Fast, 2=Turbo, 4=Slow, 8=Ultra, 31=All
 *
 * The WAV is loaded into the END of a GFSK8_RX_SAMPLE_SIZE (720000-sample)
 * buffer, matching the ring-buffer snapshot layout used by js8rx.
 * If the WAV is shorter than GFSK8_RX_SAMPLE_SIZE, it is zero-padded at the
 * front; if longer it is truncated to GFSK8_RX_SAMPLE_SIZE.
 */

#include "gfsk8modem.h"
#include "../src/constants.h"   // GFSK8_RX_SAMPLE_RATE, GFSK8_RX_SAMPLE_SIZE
#include "../src/DecodedText.h" // varicode unpacking
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Minimal WAV parser — handles PCM 16-bit mono only.
static bool read_wav(const char *path, std::vector<int16_t> &out, int &rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }

    auto r16 = [&]() -> uint16_t {
        uint8_t b[2]; (void)fread(b, 1, 2, f);
        return (uint16_t)(b[0] | b[1] << 8);
    };
    auto r32 = [&]() -> uint32_t {
        uint8_t b[4]; (void)fread(b, 1, 4, f);
        return (uint32_t)(b[0] | b[1]<<8 | b[2]<<16 | b[3]<<24);
    };
    auto id = [&](const char *s) -> bool {
        char buf[4]; (void)fread(buf, 1, 4, f);
        return memcmp(buf, s, 4) == 0;
    };

    if (!id("RIFF")) { fprintf(stderr, "not a RIFF file\n"); fclose(f); return false; }
    r32(); // file size
    if (!id("WAVE")) { fprintf(stderr, "not a WAVE file\n"); fclose(f); return false; }

    bool found_fmt = false, found_data = false;
    int channels = 0, bits = 0;
    uint32_t data_bytes = 0;

    while (!feof(f)) {
        char tag[4]; if (fread(tag, 1, 4, f) < 4) break;
        uint32_t sz = r32();
        if (memcmp(tag, "fmt ", 4) == 0) {
            uint16_t fmt = r16(); // 1=PCM
            channels = r16();
            rate = (int)r32();
            r32(); r16(); // byte rate, block align
            bits = r16();
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
            found_fmt = true;
            if (fmt != 1) { fprintf(stderr, "not PCM\n"); fclose(f); return false; }
            if (bits != 16) { fprintf(stderr, "not 16-bit\n"); fclose(f); return false; }
            if (channels != 1) { fprintf(stderr, "not mono\n"); fclose(f); return false; }
        } else if (memcmp(tag, "data", 4) == 0) {
            data_bytes = sz;
            found_data = true;
            break;
        } else {
            fseek(f, sz, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        fprintf(stderr, "missing fmt/data chunk\n"); fclose(f); return false;
    }

    uint32_t n = data_bytes / 2;
    out.resize(n);
    fread(out.data(), 2, n, f);
    fclose(f);
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.wav> [submode]\n", argv[0]);
        return 1;
    }

    int submode_mask = (argc >= 3) ? atoi(argv[2]) : gfsk8::AllSubmodes;

    std::vector<int16_t> wav;
    int rate = 0;
    if (!read_wav(argv[1], wav, rate)) return 1;

    if (rate != GFSK8_RX_SAMPLE_RATE) {
        fprintf(stderr, "warning: sample rate %d, expected %d\n", rate, GFSK8_RX_SAMPLE_RATE);
    }

    printf("loaded %zu samples (%.2f s) at %d Hz\n",
           wav.size(), wav.size() / (double)rate, rate);

    // Build a GFSK8_RX_SAMPLE_SIZE snapshot with audio at the END.
    // This matches the ring-buffer snapshot layout: oldest at index 0,
    // newest at index GFSK8_RX_SAMPLE_SIZE - 1.
    std::vector<int16_t> snap(GFSK8_RX_SAMPLE_SIZE, 0);
    size_t copy_n = std::min(wav.size(), (size_t)GFSK8_RX_SAMPLE_SIZE);
    // Place at the end
    size_t offset = (size_t)GFSK8_RX_SAMPLE_SIZE - copy_n;
    memcpy(snap.data() + offset, wav.data(), copy_n * 2);

    gfsk8::Decoder decoder(submode_mask);

    int nutc = 0; // doesn't affect decode, only stored in result
    int n_decoded = 0;

    decoder.decode(
        std::span<const int16_t>(snap.data(), snap.size()),
        nutc,
        [&](const gfsk8::Decoded &d) {
            DecodedText dt(d.message, d.frameType, d.submode);
            printf("DECODED  submode=%d  snr=%+d dB  freq=%.1f Hz  dt=%.2f s  q=%.2f  raw=[%s]  text=[%s]\n",
                   d.submode, d.snrDb, d.frequencyHz, d.dtSeconds,
                   d.quality, d.message.c_str(), dt.message().c_str());
            ++n_decoded;
        });

    printf("%d frame(s) decoded\n", n_decoded);
    return n_decoded > 0 ? 0 : 1;
}
