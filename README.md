# gfsk8-modem-clean

A complete ground-up C++20 rewrite of the JS8/FT8 modem, built from first principles
against the published specifications and the original WSJT-X research. Full JS8Call
protocol compatibility with a clean, layered architecture and zero external dependencies
beyond KissFFT (vendored).

This is the successor to [gfsk8-modem](https://github.com/jfrancis42/gfsk8-modem).
It is the modem library used by [JF8Call](https://github.com/jfrancis42/jf8call).

---

## What it provides

- **Encode** — pack a human-readable JS8 message into physical-layer frames
- **Modulate** — convert frames to 12 kHz float32 PCM audio, ready for soundcard or SDR
- **Decode** — feed 12 kHz int16 audio; receive decoded frames via callback
- All five JS8 submodes: Normal (15 s), Fast (10 s), Turbo (6 s), Slow (30 s), Ultra (4 s)
- LDPC soft-combining, frequency tracking, timing recovery
- Full JS8Call interoperability at the physical layer

No Qt. No FFTW3. No Boost. No Eigen. KissFFT is vendored in `vendor/kissfft/`.

---

## Building

```bash
git clone https://github.com/jfrancis42/gfsk8-modem-clean
cd gfsk8-modem-clean
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
# Result: build/libgfsk8modem.a
ctest --test-dir build
```

Cross-compile for Raspberry Pi (aarch64):

```bash
cmake -B build-rpi -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-raspberrypi-aarch64.cmake
cmake --build build-rpi -j2
```

Cross-compile for Windows (mingw64):

```bash
cmake -B build-windows -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-mingw64.cmake
cmake --build build-windows -j2
```

---

## Using the library

The public header is `include/gfsk8modem.h`. All symbols are in the `gfsk8` namespace.
Link with `build/libgfsk8modem.a`.

### CMake integration

```cmake
add_subdirectory(path/to/gfsk8-modem-clean)
target_link_libraries(your_target PRIVATE gfsk8modem)
```

### Encode and transmit

```cpp
#include "gfsk8modem.h"

// Pack a human-readable message into physical-layer frames
auto frames = gfsk8::pack("W5XYZ", "DM79AA", "CQ CQ CQ");

for (auto const &frame : frames) {
    // Modulate to 12 kHz float32 PCM (includes start-delay silence)
    auto pcm = gfsk8::modulate(gfsk8::Submode::Normal,
                                frame.frameType,
                                frame.payload,
                                1500.0 /* carrier Hz */);
    // Feed pcm to your audio output
}
```

### Decode

```cpp
#include "gfsk8modem.h"

gfsk8::Decoder dec(gfsk8::AllSubmodes);  // create once, reuse each period

// Called once per period at the UTC period boundary:
dec.decode(audio_ring_buffer, nutc, [](gfsk8::Decoded const &d) {
    printf("Decoded: %s  SNR=%d dB  freq=%.1f Hz\n",
           d.message.c_str(), d.snrDb, d.frequencyHz);
});
```

The decoder accepts a 12 kHz int16 ring buffer sized to `GFSK8_RX_SAMPLE_SIZE`
(720 000 samples = 60 seconds). Fill it continuously from your audio source and
call `decode()` once at each UTC period boundary.

---

## JS8 Submode Parameters

| Submode | Period | Tone spacing | SNR threshold |
|---------|--------|--------------|---------------|
| Normal (JS8A) | 15 s | 6.25 Hz | −24 dB |
| Fast (JS8B) | 10 s | 10.0 Hz | −22 dB |
| Turbo (JS8C) | 6 s | 20.0 Hz | −20 dB |
| Slow (JS8E) | 30 s | 3.125 Hz | −28 dB |
| Ultra (JS8I) | 4 s | 31.25 Hz | −18 dB |

Audio format: 12 000 Hz, int16 (RX input) / float32 (TX output).

---

## Demo applications

`apps/js8rx/` — live multi-frequency JS8 receiver using PortAudio (built automatically
when PortAudio is found).

`apps/js8tx/` — encode a message and play it to the soundcard.

---

## License

GPL-3.0. The JS8 protocol and encoding scheme were developed by Jordan Sherer KN4CRD,
based on WSJT-X by K1JT (Joe Taylor) and the broader weak-signal digital modes community.

Any project that statically links `libgfsk8modem` must also be distributed under
GPL-3.0 or a compatible license.
