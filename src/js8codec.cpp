/**
 * @file js8codec.cpp
 * @brief JS8 modem: encoder and multi-mode decoder.
 *
 * This is a ground-up C++20 implementation of the JS8 physical layer.
 * Algorithm fidelity is maintained with the JS8Call-improved C++ port
 * (originally translated from Fortran by Allan Bazinet W6BAZ).
 *
 * Key departures from the reference implementation:
 *   - Zero Qt dependency (QThread/QDebug replaced by std:: primitives).
 *   - FFTW3 replaced by KissFFT (via fft_shim.h).
 *   - Boost.MultiIndex replaced by std::vector + sort-on-demand.
 *   - Boost.CRC replaced by a 30-line bit-at-a-time CRC-12 (crc12.h).
 *   - Eigen removed entirely (Gaussian elimination in plain C++).
 *   - No global mutable state except g_decodeData / g_fftMutex (constants.h).
 */

#include "js8codec.h"
#include "constants.h"
#include "crc12.h"
#include "fft_shim.h"
#include "ldpc_feedback.h"
#include "log.h"
#include "soft_combiner.h"
#include "tracker.h"
#include "whitening.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(decoder_js8)

// ── Kahan summation ──────────────────────────────────────────────────────────
// Used to match the Fortran Nuttall-window calculation precision.
namespace {
template <std::floating_point T>
class KahanSum {
    T m_sum{};
    T m_c{};
public:
    KahanSum(T v = T{}) : m_sum(v) {}
    KahanSum &operator=(T v) { m_sum = v; m_c = T{}; return *this; }
    KahanSum &operator+=(T v) {
        T y = v - m_c;
        T t = m_sum + y;
        m_c = (t - m_sum) - y;
        m_sum = t;
        return *this;
    }
    operator T() const { return m_sum; }
};
} // namespace

// ── Protocol constants ────────────────────────────────────────────────────────
namespace {
constexpr int   N        = 174;   // total codeword bits
constexpr int   K        = 87;    // message bits
constexpr int   M        = N - K; // parity-check bits
constexpr int   KK       = 87;    // information bits (72 payload + 3 type + 12 CRC)
constexpr int   ND       = 58;    // data symbols
constexpr int   NS       = 21;    // sync symbols (3 × 7 Costas)
constexpr int   NN       = NS + ND; // 79
constexpr float ASYNCMIN = 1.5f;
constexpr int   NFSRCH   = 5;
constexpr std::size_t NMAXCAND = 300;
constexpr int   NFILT    = 1400;
constexpr int   NROWS    = 8;
constexpr int   NFOS     = 2;
constexpr int   NSSY     = 4;
constexpr float TAU      = 2.0f * std::numbers::pi_v<float>;
constexpr auto  ZERO     = std::complex<float>{0.0f, 0.0f};
} // namespace

// ── Costas arrays (compile-time) ─────────────────────────────────────────────
namespace {
// ORIGINAL: Normal mode (same as upstream FT8).
constexpr gfsk8inner::Costas::Array COSTAS_ORIGINAL = {{
    {4, 2, 5, 6, 1, 3, 0},
    {4, 2, 5, 6, 1, 3, 0},
    {4, 2, 5, 6, 1, 3, 0}
}};

// MODIFIED: Fast / Turbo / Slow / Ultra modes.
constexpr gfsk8inner::Costas::Array COSTAS_MODIFIED = {{
    {0, 6, 2, 3, 5, 4, 1},
    {1, 5, 0, 2, 3, 6, 4},
    {2, 5, 0, 6, 4, 1, 3}
}};
} // namespace

gfsk8inner::Costas::Array const &gfsk8inner::Costas::get(Type type)
{
    return (type == Type::ORIGINAL) ? COSTAS_ORIGINAL : COSTAS_MODIFIED;
}

// ── JS8 alphabet ─────────────────────────────────────────────────────────────
namespace {
constexpr std::string_view ALPHABET =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";
static_assert(ALPHABET.size() == 64);

// Lookup table: character → 6-bit word; 0xFF marks invalid characters.
constexpr auto makeAlphabetTable()
{
    std::array<uint8_t, 256> t{};
    t.fill(0xFF);
    for (std::size_t i = 0; i < ALPHABET.size(); ++i)
        t[static_cast<uint8_t>(ALPHABET[i])] = static_cast<uint8_t>(i);
    return t;
}
constexpr auto ALPHABET_TABLE = makeAlphabetTable();

uint8_t alphabetWord(char c)
{
    uint8_t const v = ALPHABET_TABLE[static_cast<uint8_t>(c)];
    if (v == 0xFF) throw std::runtime_error("Invalid JS8 character");
    return v;
}

// ── CRC-12 helpers ────────────────────────────────────────────────────────────

template <typename Arr>
uint16_t CRC12(Arr const &arr)
{
    return crc12::compute(reinterpret_cast<const uint8_t *>(arr.data()),
                          arr.size()) ^ 42;
}

bool checkCRC12(std::array<int8_t, KK> const &decoded)
{
    std::array<uint8_t, 11> bits{};
    for (std::size_t i = 0; i < static_cast<std::size_t>(KK); ++i)
        if (decoded[i]) bits[i / 8] |= static_cast<uint8_t>(1u << (7 - (i % 8)));

    uint16_t const rx = (static_cast<uint16_t>(bits[9] & 0x1Fu) << 7) |
                        (static_cast<uint16_t>(bits[10]) >> 1);
    bits[9]  &= 0xE0u;
    bits[10]  = 0u;
    return rx == CRC12(bits);
}

std::string extractMessage(std::array<int8_t, KK> const &decoded)
{
    if (!checkCRC12(decoded)) return {};
    std::array<uint8_t, 12> words;
    for (std::size_t i = 0; i < 12; ++i)
        words[i] = static_cast<uint8_t>(
            (decoded[i*6+0] << 5) | (decoded[i*6+1] << 4) |
            (decoded[i*6+2] << 3) | (decoded[i*6+3] << 2) |
            (decoded[i*6+4] << 1) | decoded[i*6+5]);
    std::string msg;
    msg.reserve(12);
    for (auto w : words) msg += ALPHABET[w];
    return msg;
}

// ── LDPC parity matrix ────────────────────────────────────────────────────────
// 87×87 bit-packed generator matrix (MSB-first per row).
// Hex strings from JS8Call-improved's ldpc_174_87_params.f90, reordered for
// C++ row-major layout.
constexpr auto buildParity()
{
    constexpr std::size_t Rows = 87;
    constexpr std::size_t Cols = 87;
    constexpr std::array<std::string_view, Rows> Data = {
        "23bba830e23b6b6f50982e", "1f8e55da218c5df3309052",
        "ca7b3217cd92bd59a5ae20", "56f78313537d0f4382964e",
        "6be396b5e2e819e373340c", "293548a138858328af4210",
        "cb6c6afcdc28bb3f7c6e86", "3f2a86f5c5bd225c961150",
        "849dd2d63673481860f62c", "56cdaec6e7ae14b43feeee",
        "04ef5cfa3766ba778f45a4", "c525ae4bd4f627320a3974",
        "41fd9520b2e4abeb2f989c", "7fb36c24085a34d8c1dbc4",
        "40fc3e44bb7d2bb2756e44", "d38ab0a1d2e52a8ec3bc76",
        "3d0f929ef3949bd84d4734", "45d3814f504064f80549ae",
        "f14dbf263825d0bd04b05e", "db714f8f64e8ac7af1a76e",
        "8d0274de71e7c1a8055eb0", "51f81573dd4049b082de14",
        "d8f937f31822e57c562370", "b6537f417e61d1a7085336",
        "ecbd7c73b9cd34c3720c8a", "3d188ea477f6fa41317a4e",
        "1ac4672b549cd6dba79bcc", "a377253773ea678367c3f6",
        "0dbd816fba1543f721dc72", "ca4186dd44c3121565cf5c",
        "29c29dba9c545e267762fe", "1616d78018d0b4745ca0f2",
        "fe37802941d66dde02b99c", "a9fa8e50bcb032c85e3304",
        "83f640f1a48a8ebc0443ea", "3776af54ccfbae916afde6",
        "a8fc906976c35669e79ce0", "f08a91fb2e1f78290619a8",
        "cc9da55fe046d0cb3a770c", "d36d662a69ae24b74dcbd8",
        "40907b01280f03c0323946", "d037db825175d851f3af00",
        "1bf1490607c54032660ede", "0af7723161ec223080be86",
        "eca9afa0f6b01d92305edc", "7a8dec79a51e8ac5388022",
        "9059dfa2bb20ef7ef73ad4", "6abb212d9739dfc02580f2",
        "f6ad4824b87c80ebfce466", "d747bfc5fd65ef70fbd9bc",
        "612f63acc025b6ab476f7c", "05209a0abb530b9e7e34b0",
        "45b7ab6242b77474d9f11a", "6c280d2a0523d9c4bc5946",
        "f1627701a2d692fd9449e6", "8d9071b7e7a6a2eed6965e",
        "bf4f56e073271f6ab4bf80", "c0fc3ec4fb7d2bb2756644",
        "57da6d13cb96a7689b2790", "a9fa2eefa6f8796a355772",
        "164cc861bdd803c547f2ac", "cc6de59755420925f90ed2",
        "a0c0033a52ab6299802fd2", "b274db8abd3c6f396ea356",
        "97d4169cb33e7435718d90", "81cfc6f18c35b1e1f17114",
        "481a2a0df8a23583f82d6c", "081c29a10d468ccdbcecb6",
        "2c4142bf42b01e71076acc", "a6573f3dc8b16c9d19f746",
        "c87af9a5d5206abca532a8", "012dee2198eba82b19a1da",
        "b1ca4ea2e3d173bad4379c", "b33ec97be83ce413f9acc8",
        "5b0f7742bca86b8012609a", "37d8e0af9258b9e8c5f9b2",
        "35ad3fb0faeb5f1b0c30dc", "6114e08483043fd3f38a8a",
        "cd921fdf59e882683763f6", "95e45ecd0135aca9d6e6ae",
        "2e547dd7a05f6597aac516", "14cd0f642fc0c5fe3a65ca",
        "3a0a1dfd7eee29c2e827e0", "c8b5dffc335095dcdcaf2a",
        "3dd01a59d86310743ec752", "8abdb889efbe39a510a118",
        "3f231f212055371cf3e2a2"
    };

    using E = uint64_t;
    constexpr std::size_t EBits = std::numeric_limits<E>::digits;
    constexpr std::size_t Total = (Rows * Cols + EBits - 1);
    constexpr std::size_t Count = Total / EBits;

    std::array<E, Count> data{};

    for (std::size_t row = 0; row < Rows; ++row) {
        std::size_t col = 0;
        for (char c : Data[row]) {
            uint8_t nib = (c >= '0' && c <= '9') ? static_cast<uint8_t>(c - '0')
                        : (c >= 'a' && c <= 'f') ? static_cast<uint8_t>(c - 'a' + 10)
                        : static_cast<uint8_t>(c - 'A' + 10);
            for (int b = 3; b >= 0; --b) {
                if (col >= Cols) break;
                if (nib & (1u << b)) {
                    std::size_t idx = row * Cols + col;
                    data[idx / EBits] |= (E(1) << (idx % EBits));
                }
                ++col;
            }
        }
    }
    return data;
}

constexpr auto PARITY_DATA = buildParity();

constexpr bool parity(std::size_t row, std::size_t col)
{
    constexpr std::size_t Cols = 87;
    constexpr std::size_t EBits = std::numeric_limits<uint64_t>::digits;
    std::size_t idx = row * Cols + col;
    return (PARITY_DATA[idx / EBits] >> (idx % EBits)) & 1u;
}

// ── Belief-propagation (BP) LDPC decoder ─────────────────────────────────────
// Parity-check and variable-node connection tables from ldpc_174_87_params.f90.

constexpr int BP_MAX_ROWS   = 7;
constexpr int BP_MAX_CHECKS = 3;
constexpr int BP_MAX_ITER   = 30;

constexpr std::array<std::array<int, BP_MAX_CHECKS>, N> Mn = {{
    {{0,24,68}},  {{1,4,72}},   {{2,31,67}},  {{3,50,60}},  {{5,62,69}},
    {{6,32,78}},  {{7,49,85}},  {{8,36,42}},  {{9,40,64}},  {{10,13,63}},
    {{11,74,76}}, {{12,22,80}}, {{14,15,81}}, {{16,55,65}}, {{17,52,59}},
    {{18,30,51}}, {{19,66,83}}, {{20,28,71}}, {{21,23,43}}, {{25,34,75}},
    {{26,35,37}}, {{27,39,41}}, {{29,53,54}}, {{33,48,86}}, {{38,56,57}},
    {{44,73,82}}, {{45,61,79}}, {{46,47,84}}, {{58,70,77}}, {{0,49,52}},
    {{1,46,83}},  {{2,24,78}},  {{3,5,13}},   {{4,6,79}},   {{7,33,54}},
    {{8,35,68}},  {{9,42,82}},  {{10,22,73}}, {{11,16,43}}, {{12,56,75}},
    {{14,26,55}}, {{15,27,28}}, {{17,18,58}}, {{19,39,62}}, {{20,34,51}},
    {{21,53,63}}, {{23,61,77}}, {{25,31,76}}, {{29,71,84}}, {{30,64,86}},
    {{32,38,50}}, {{36,47,74}}, {{37,69,70}}, {{40,41,67}}, {{44,66,85}},
    {{45,80,81}}, {{48,65,72}}, {{57,59,65}}, {{60,64,84}}, {{0,13,20}},
    {{1,12,58}},  {{2,66,81}},  {{3,31,72}},  {{4,35,53}},  {{5,42,45}},
    {{6,27,74}},  {{7,32,70}},  {{8,48,75}},  {{9,57,63}},  {{10,47,67}},
    {{11,18,44}}, {{14,49,60}}, {{15,21,25}}, {{16,71,79}}, {{17,39,54}},
    {{19,34,50}}, {{22,24,33}}, {{23,62,86}}, {{26,38,73}}, {{28,77,82}},
    {{29,69,76}}, {{30,68,83}}, {{21,36,85}}, {{37,40,80}}, {{41,43,56}},
    {{46,52,61}}, {{51,55,78}}, {{59,74,80}}, {{0,38,76}},  {{1,15,40}},
    {{2,30,53}},  {{3,35,77}},  {{4,44,64}},  {{5,56,84}},  {{6,13,48}},
    {{7,20,45}},  {{8,14,71}},  {{9,19,61}},  {{10,16,70}}, {{11,33,46}},
    {{12,67,85}}, {{17,22,42}}, {{18,63,72}}, {{23,47,78}}, {{24,69,82}},
    {{25,79,86}}, {{26,31,39}}, {{27,55,68}}, {{28,62,65}}, {{29,41,49}},
    {{32,36,81}}, {{34,59,73}}, {{37,54,83}}, {{43,51,60}}, {{50,52,71}},
    {{57,58,66}}, {{46,55,75}}, {{0,18,36}},  {{1,60,74}},  {{2,7,65}},
    {{3,59,83}},  {{4,33,38}},  {{5,25,52}},  {{6,31,56}},  {{8,51,66}},
    {{9,11,14}},  {{10,50,68}}, {{12,13,64}}, {{15,30,42}}, {{16,19,35}},
    {{17,79,85}}, {{20,47,58}}, {{21,39,45}}, {{22,32,61}}, {{23,29,73}},
    {{24,41,63}}, {{26,48,84}}, {{27,37,72}}, {{28,43,80}}, {{34,67,69}},
    {{40,62,75}}, {{44,48,70}}, {{49,57,86}}, {{47,53,82}}, {{12,54,78}},
    {{76,77,81}}, {{0,1,23}},   {{2,5,74}},   {{3,55,86}},  {{4,43,52}},
    {{6,49,82}},  {{7,9,27}},   {{8,54,61}},  {{10,28,66}}, {{11,32,39}},
    {{13,15,19}}, {{14,34,72}}, {{16,30,38}}, {{17,35,56}}, {{18,45,75}},
    {{20,41,83}}, {{21,33,58}}, {{22,25,60}}, {{24,59,64}}, {{26,63,79}},
    {{29,36,65}}, {{31,44,71}}, {{37,50,85}}, {{40,76,78}}, {{42,55,67}},
    {{46,73,81}}, {{39,51,77}}, {{53,60,70}}, {{45,57,68}}
}};

struct CheckNode {
    int count;
    std::array<int, BP_MAX_ROWS> nb;
};

constexpr std::array<CheckNode, M> Nm = {{
    {6,{0,29,59,88,117,146,0}},   {6,{1,30,60,89,118,146,0}},
    {6,{2,31,61,90,119,147,0}},   {6,{3,32,62,91,120,148,0}},
    {6,{1,33,63,92,121,149,0}},   {6,{4,32,64,93,122,147,0}},
    {6,{5,33,65,94,123,150,0}},   {6,{6,34,66,95,119,151,0}},
    {6,{7,35,67,96,124,152,0}},   {6,{8,36,68,97,125,151,0}},
    {6,{9,37,69,98,126,153,0}},   {6,{10,38,70,99,125,154,0}},
    {6,{11,39,60,100,127,144,0}}, {6,{9,32,59,94,127,155,0}},
    {6,{12,40,71,96,125,156,0}},  {6,{12,41,72,89,128,155,0}},
    {6,{13,38,73,98,129,157,0}},  {6,{14,42,74,101,130,158,0}},
    {6,{15,42,70,102,117,159,0}}, {6,{16,43,75,97,129,155,0}},
    {6,{17,44,59,95,131,160,0}},  {6,{18,45,72,82,132,161,0}},
    {6,{11,37,76,101,133,162,0}}, {6,{18,46,77,103,134,146,0}},
    {6,{0,31,76,104,135,163,0}},  {6,{19,47,72,105,122,162,0}},
    {6,{20,40,78,106,136,164,0}}, {6,{21,41,65,107,137,151,0}},
    {6,{17,41,79,108,138,153,0}}, {6,{22,48,80,109,134,165,0}},
    {6,{15,49,81,90,128,157,0}},  {6,{2,47,62,106,123,166,0}},
    {6,{5,50,66,110,133,154,0}},  {6,{23,34,76,99,121,161,0}},
    {6,{19,44,75,111,139,156,0}}, {6,{20,35,63,91,129,158,0}},
    {6,{7,51,82,110,117,165,0}},  {6,{20,52,83,112,137,167,0}},
    {6,{24,50,78,88,121,157,0}},  {7,{21,43,74,106,132,154,171}},
    {6,{8,53,83,89,140,168,0}},   {6,{21,53,84,109,135,160,0}},
    {6,{7,36,64,101,128,169,0}},  {6,{18,38,84,113,138,149,0}},
    {6,{25,54,70,92,141,166,0}},  {7,{26,55,64,95,132,159,173}},
    {6,{27,30,85,99,116,170,0}},  {6,{27,51,69,103,131,143,0}},
    {6,{23,56,67,94,136,141,0}},  {6,{6,29,71,109,142,150,0}},
    {6,{3,50,75,114,126,167,0}},  {6,{15,44,86,113,124,171,0}},
    {6,{14,29,85,114,122,149,0}}, {6,{22,45,63,90,143,172,0}},
    {6,{22,34,74,112,144,152,0}}, {7,{13,40,86,107,116,148,169}},
    {6,{24,39,84,93,123,158,0}},  {6,{24,57,68,115,142,173,0}},
    {6,{28,42,60,115,131,161,0}}, {6,{14,57,87,111,120,163,0}},
    {7,{3,58,71,113,118,162,172}},{6,{26,46,85,97,133,152,0}},
    {5,{4,43,77,108,140,0,0}},    {6,{9,45,68,102,135,164,0}},
    {6,{8,49,58,92,127,163,0}},   {6,{13,56,57,108,119,165,0}},
    {6,{16,54,61,115,124,153,0}}, {6,{2,53,69,100,139,169,0}},
    {6,{0,35,81,107,126,173,0}},  {5,{4,52,80,104,139,0,0}},
    {6,{28,52,66,98,141,172,0}},  {6,{17,48,73,96,114,166,0}},
    {6,{1,56,62,102,137,156,0}},  {6,{25,37,78,111,134,170,0}},
    {6,{10,51,65,87,118,147,0}},  {6,{19,39,67,116,140,159,0}},
    {6,{10,47,80,88,145,168,0}},  {6,{28,46,79,91,145,171,0}},
    {6,{5,31,86,103,144,168,0}},  {6,{26,33,73,105,130,164,0}},
    {5,{11,55,83,87,138,0,0}},    {6,{12,55,61,110,145,170,0}},
    {6,{25,36,79,104,143,150,0}}, {6,{16,30,81,112,120,160,0}},
    {5,{27,48,58,93,136,0,0}},    {6,{6,54,82,100,130,167,0}},
    {6,{23,49,77,105,142,148,0}}
}};

int bpdecode174(std::array<float, N> const &llr,
                std::array<int8_t, K> &decoded,
                std::array<int8_t, N> &cw)
{
    std::array<std::array<float, BP_MAX_CHECKS>, N> tov{};
    std::array<std::array<float, BP_MAX_ROWS>,   M> toc{};
    std::array<std::array<float, BP_MAX_ROWS>,   M> tanhtoc{};
    std::array<float, N> zn{};
    std::array<int,   M> synd{};

    int ncnt = 0, nclast = 0;

    // Initialize toc from prior LLRs.
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < Nm[i].count; ++j)
            toc[i][j] = llr[Nm[i].nb[j]];

    for (int iter = 0; iter <= BP_MAX_ITER; ++iter) {
        // Update bit LLRs.
        for (int i = 0; i < N; ++i)
            zn[i] = llr[i] + std::accumulate(tov[i].begin(),
                                              tov[i].begin() + BP_MAX_CHECKS, 0.0f);

        // Hard decision.
        for (int i = 0; i < N; ++i) cw[i] = (zn[i] > 0) ? 1 : 0;

        // Syndrome check.
        int ncheck = 0;
        for (int i = 0; i < M; ++i) {
            int s = 0;
            for (int j = 0; j < Nm[i].count; ++j) s += cw[Nm[i].nb[j]];
            synd[i] = s;
            if (s % 2 != 0) ++ncheck;
        }

        if (ncheck == 0) {
            std::copy(cw.begin() + M, cw.end(), decoded.begin());
            int nerr = 0;
            for (int i = 0; i < N; ++i)
                if ((2 * cw[i] - 1) * llr[i] < 0.0f) ++nerr;
            return nerr;
        }

        // Early stopping.
        if (iter > 0) {
            int nd = ncheck - nclast;
            ncnt = (nd < 0) ? 0 : ncnt + 1;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) return -1;
        }
        nclast = ncheck;

        // Messages: bits → check nodes.
        for (int i = 0; i < M; ++i)
            for (int j = 0; j < Nm[i].count; ++j) {
                int ibj = Nm[i].nb[j];
                toc[i][j] = zn[ibj];
                for (int k = 0; k < BP_MAX_CHECKS; ++k)
                    if (Mn[ibj][k] == i) toc[i][j] -= tov[ibj][k];
            }

        // Tanh of check-node messages.
        for (int i = 0; i < M; ++i)
            for (int j = 0; j < 7; ++j)
                tanhtoc[i][j] = std::tanh(-toc[i][j] / 2.0f);

        // Messages: check nodes → bits.
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < BP_MAX_CHECKS; ++j) {
                int ichk = Mn[i][j];
                float Tmn = 1.0f;
                for (int k = 0; k < Nm[ichk].count; ++k)
                    if (Nm[ichk].nb[k] != i) Tmn *= tanhtoc[ichk][k];
                tov[i][j] = 2.0f * std::atanh(-Tmn);
            }
    }
    return -1;
}

} // anonymous namespace

// ── Decoded message record ────────────────────────────────────────────────────
namespace {
struct DecodeResult {
    int         type;
    std::string data;

    bool operator==(DecodeResult const &o) const noexcept = default;

    struct Hash {
        std::size_t operator()(DecodeResult const &d) const noexcept {
            return std::hash<int>{}(d.type) ^
                   (std::hash<std::string>{}(d.data) + 0x9e3779b9u +
                    (std::hash<int>{}(d.type) << 6) + (std::hash<int>{}(d.type) >> 2));
        }
    };
    using Map = std::unordered_map<DecodeResult, int, Hash>;
};

struct SyncCandidate {
    float freq;
    float step;
    float sync;
};

} // namespace

// ── Compile-time constexpr floor ──────────────────────────────────────────────
namespace {
template <std::floating_point T>
constexpr int cfloor(T v) {
    auto i = static_cast<int>(v);
    return (v < i) ? i - 1 : i;
}

// Constexpr cosine approximation (used for compile-time taper arrays).
constexpr double cos_approx(double x) {
    constexpr double PI2  = std::numbers::pi * 2.0;
    constexpr double PI   = std::numbers::pi;
    constexpr double PI_2 = std::numbers::pi / 2.0;
    x -= static_cast<long long>(x / PI2) * PI2;
    if (x > PI) x = PI2 - x;
    bool neg = (x > PI_2);
    if (neg) x = PI - x;
    // Taylor polynomial for cos in [0, π/2]
    double x2 = x*x;
    double r = 1.0 - x2*(0.5 - x2*(1.0/24 - x2*(1.0/720 - x2*(1.0/40320
               - x2*(1.0/3628800 - x2*(1.0/479001600
               - x2*(1.0/87178291200.0 - x2/20922789888000.0)))))));
    return neg ? -r : r;
}
} // namespace

// ── Baseline polynomial constants ─────────────────────────────────────────────
namespace {
constexpr int    BASELINE_DEGREE = 5;
constexpr int    BASELINE_SAMPLE = 10;
constexpr int    BASELINE_N      = BASELINE_DEGREE + 1;
constexpr int    BASELINE_MIN    = 500;
constexpr int    BASELINE_MAX    = 2500;

constexpr auto BASELINE_NODES = []() {
    std::array<double, BASELINE_N> nodes{};
    constexpr double slice = std::numbers::pi / (2.0 * BASELINE_N);
    for (int i = 0; i < BASELINE_N; ++i)
        nodes[i] = 0.5 * (1.0 - cos_approx(slice * (2.0 * i + 1)));
    return nodes;
}();
} // namespace

// ── Per-mode constants ────────────────────────────────────────────────────────
namespace {

struct ModeA {
    static constexpr int   NSUBMODE = 0;
    static constexpr auto  NCOSTAS  = gfsk8inner::Costas::Type::ORIGINAL;
    static constexpr int   NSPS     = GFSK8_NORMAL_SYMBOL_SAMPLES;
    static constexpr int   NTXDUR   = GFSK8_NORMAL_TX_SECONDS;
    static constexpr int   NDOWNSPS = 32;
    static constexpr int   NDD      = 100;
    static constexpr int   JZ       = 62;
    static constexpr float ASTART   = 0.5f;
    static constexpr float BASESUB  = 40.0f;
    static constexpr float AZ       = (12000.0f / NSPS) * 0.64f;
    static constexpr int   NMAX     = NTXDUR * GFSK8_RX_SAMPLE_RATE;
    static constexpr int   NFFT1    = NSPS * NFOS;
    static constexpr int   NSTEP    = NSPS / NSSY;
    static constexpr int   NHSYM    = NMAX / NSTEP - 3;
    static constexpr int   NDOWN    = NSPS / NDOWNSPS;
    static constexpr int   NQSYMBOL = NDOWNSPS / 4;
    static constexpr int   NDFFT1   = NSPS * NDD;
    static constexpr int   NDFFT2   = NDFFT1 / NDOWN;
    static constexpr int   NP2      = NN * NDOWNSPS;
    static constexpr float TSTEP    = static_cast<float>(NSTEP) / 12000.0f;
    static constexpr int   JSTRT    = static_cast<int>(ASTART / TSTEP);
    static constexpr float DF       = 12000.0f / NFFT1;
};

struct ModeB {
    static constexpr int   NSUBMODE = 1;
    static constexpr auto  NCOSTAS  = gfsk8inner::Costas::Type::MODIFIED;
    static constexpr int   NSPS     = GFSK8_FAST_SYMBOL_SAMPLES;
    static constexpr int   NTXDUR   = GFSK8_FAST_TX_SECONDS;
    static constexpr int   NDOWNSPS = 20;
    static constexpr int   NDD      = 100;
    static constexpr int   JZ       = 144;
    static constexpr float ASTART   = 0.2f;
    static constexpr float BASESUB  = 39.0f;
    static constexpr float AZ       = (12000.0f / NSPS) * 0.8f;
    static constexpr int   NMAX     = NTXDUR * GFSK8_RX_SAMPLE_RATE;
    static constexpr int   NFFT1    = NSPS * NFOS;
    static constexpr int   NSTEP    = NSPS / NSSY;
    static constexpr int   NHSYM    = NMAX / NSTEP - 3;
    static constexpr int   NDOWN    = NSPS / NDOWNSPS;
    static constexpr int   NQSYMBOL = NDOWNSPS / 4;
    static constexpr int   NDFFT1   = NSPS * NDD;
    static constexpr int   NDFFT2   = NDFFT1 / NDOWN;
    static constexpr int   NP2      = NN * NDOWNSPS;
    static constexpr float TSTEP    = static_cast<float>(NSTEP) / 12000.0f;
    static constexpr int   JSTRT    = static_cast<int>(ASTART / TSTEP);
    static constexpr float DF       = 12000.0f / NFFT1;
};

struct ModeC {
    static constexpr int   NSUBMODE = 2;
    static constexpr auto  NCOSTAS  = gfsk8inner::Costas::Type::MODIFIED;
    static constexpr int   NSPS     = GFSK8_TURBO_SYMBOL_SAMPLES;
    static constexpr int   NTXDUR   = GFSK8_TURBO_TX_SECONDS;
    static constexpr int   NDOWNSPS = 12;
    static constexpr int   NDD      = 120;
    static constexpr int   JZ       = 172;
    static constexpr float ASTART   = 0.1f;
    static constexpr float BASESUB  = 38.0f;
    static constexpr float AZ       = (12000.0f / NSPS) * 0.6f;
    static constexpr int   NMAX     = NTXDUR * GFSK8_RX_SAMPLE_RATE;
    static constexpr int   NFFT1    = NSPS * NFOS;
    static constexpr int   NSTEP    = NSPS / NSSY;
    static constexpr int   NHSYM    = NMAX / NSTEP - 3;
    static constexpr int   NDOWN    = NSPS / NDOWNSPS;
    static constexpr int   NQSYMBOL = NDOWNSPS / 4;
    static constexpr int   NDFFT1   = NSPS * NDD;
    static constexpr int   NDFFT2   = NDFFT1 / NDOWN;
    static constexpr int   NP2      = NN * NDOWNSPS;
    static constexpr float TSTEP    = static_cast<float>(NSTEP) / 12000.0f;
    static constexpr int   JSTRT    = static_cast<int>(ASTART / TSTEP);
    static constexpr float DF       = 12000.0f / NFFT1;
};

struct ModeE {
    static constexpr int   NSUBMODE = 4;
    static constexpr auto  NCOSTAS  = gfsk8inner::Costas::Type::MODIFIED;
    static constexpr int   NSPS     = GFSK8_SLOW_SYMBOL_SAMPLES;
    static constexpr int   NTXDUR   = GFSK8_SLOW_TX_SECONDS;
    static constexpr int   NDOWNSPS = 32;
    static constexpr int   NDD      = 94;
    static constexpr int   JZ       = 32;
    static constexpr float ASTART   = 0.5f;
    static constexpr float BASESUB  = 42.0f;
    static constexpr float AZ       = (12000.0f / NSPS) * 0.64f;
    static constexpr int   NMAX     = NTXDUR * GFSK8_RX_SAMPLE_RATE;
    static constexpr int   NFFT1    = NSPS * NFOS;
    static constexpr int   NSTEP    = NSPS / NSSY;
    static constexpr int   NHSYM    = NMAX / NSTEP - 3;
    static constexpr int   NDOWN    = NSPS / NDOWNSPS;
    static constexpr int   NQSYMBOL = NDOWNSPS / 4;
    static constexpr int   NDFFT1   = NSPS * NDD;
    static constexpr int   NDFFT2   = NDFFT1 / NDOWN;
    static constexpr int   NP2      = NN * NDOWNSPS;
    static constexpr float TSTEP    = static_cast<float>(NSTEP) / 12000.0f;
    static constexpr int   JSTRT    = static_cast<int>(ASTART / TSTEP);
    static constexpr float DF       = 12000.0f / NFFT1;
};

struct ModeI {
    static constexpr int   NSUBMODE = 8;
    static constexpr auto  NCOSTAS  = gfsk8inner::Costas::Type::MODIFIED;
    static constexpr int   NSPS     = GFSK8_ULTRA_SYMBOL_SAMPLES;
    static constexpr int   NTXDUR   = GFSK8_ULTRA_TX_SECONDS;
    static constexpr int   NDOWNSPS = 12;
    static constexpr int   NDD      = 125;
    static constexpr int   JZ       = 250;
    static constexpr float ASTART   = 0.1f;
    static constexpr float BASESUB  = 36.0f;
    static constexpr float AZ       = (12000.0f / NSPS) * 0.64f;
    static constexpr int   NMAX     = NTXDUR * GFSK8_RX_SAMPLE_RATE;
    static constexpr int   NFFT1    = NSPS * NFOS;
    static constexpr int   NSTEP    = NSPS / NSSY;
    static constexpr int   NHSYM    = NMAX / NSTEP - 3;
    static constexpr int   NDOWN    = NSPS / NDOWNSPS;
    static constexpr int   NQSYMBOL = NDOWNSPS / 4;
    static constexpr int   NDFFT1   = NSPS * NDD;
    static constexpr int   NDFFT2   = NDFFT1 / NDOWN;
    static constexpr int   NP2      = NN * NDOWNSPS;
    static constexpr float TSTEP    = static_cast<float>(NSTEP) / 12000.0f;
    static constexpr int   JSTRT    = static_cast<int>(ASTART / TSTEP);
    static constexpr float DF       = 12000.0f / NFFT1;
};

// ── FFT plan manager ──────────────────────────────────────────────────────────

class PlanManager {
public:
    enum class Id { DS, BB, CF, CB, SD, CS, COUNT };

    PlanManager() { m_plans.fill(nullptr); }

    ~PlanManager() {
        std::lock_guard<std::mutex> lk(g_fftMutex);
        for (auto &p : m_plans) if (p) fftwf_destroy_plan(p);
    }

    PlanManager(PlanManager const &) = delete;
    PlanManager &operator=(PlanManager const &) = delete;

    fftwf_plan &operator[](Id id) noexcept
    {
        return m_plans[static_cast<std::size_t>(id)];
    }
    fftwf_plan operator[](Id id) const noexcept
    {
        return m_plans[static_cast<std::size_t>(id)];
    }
    auto begin() noexcept { return m_plans.begin(); }
    auto end()   noexcept { return m_plans.end(); }

private:
    std::array<fftwf_plan, static_cast<std::size_t>(Id::COUNT)> m_plans;
};

// ── Per-mode decode engine ────────────────────────────────────────────────────

template <typename Mode>
class DecodeEngine {
    using Plan = PlanManager::Id;

    // Static Costas lookup — selected at compile time from the local constexpr arrays.
    static constexpr auto &Costas =
        (Mode::NCOSTAS == gfsk8inner::Costas::Type::ORIGINAL) ? COSTAS_ORIGINAL
                                                             : COSTAS_MODIFIED;

    // Compile-time tapers for downsampling edges.
    static constexpr auto Taper = []() {
        std::array<std::array<float, Mode::NDD + 1>, 2> t{};
        for (int i = 0; i <= Mode::NDD; ++i) {
            float v = 0.5f * (1.0f + static_cast<float>(
                cos_approx(i * std::numbers::pi_v<double> / Mode::NDD)));
            t[1][i]             = v;           // tail
            t[0][Mode::NDD - i] = v;           // head (reversed)
        }
        return t;
    }();

    // Member buffers.
    std::array<float,     Mode::NFFT1>                           nuttal;
    std::array<std::array<std::array<std::complex<float>,
                          Mode::NDOWNSPS>, 7>, 3>                csyncs;
    alignas(64) std::array<std::complex<float>, Mode::NDOWNSPS> csymb;
    alignas(64) std::array<std::complex<float>, Mode::NMAX>     filter;
    alignas(64) std::array<std::complex<float>, Mode::NMAX>     cfilt;
    alignas(64) std::array<std::complex<float>,
                           Mode::NDFFT1 / 2 + 1>                 ds_cx;
    alignas(64) std::array<std::complex<float>,
                           Mode::NFFT1 / 2 + 1>                  sd;
    alignas(64) std::array<std::complex<float>, 3200>            cd0;
    std::array<float, Mode::NMAX>                                dd;
    std::array<std::array<float, Mode::NHSYM>, Mode::NSPS>      s;
    std::array<float, Mode::NSPS>                                savg;
    PlanManager plans;
    gfsk8::SoftCombiner<N> softCombiner;

    bool  m_freqTracking   = (std::getenv("GFSK8_DISABLE_FREQ_TRACKING")   == nullptr);
    bool  m_timingTracking = (std::getenv("GFSK8_DISABLE_TIMING_TRACKING") == nullptr);
    float m_erasureThresh  = gfsk8::llrErasureThreshold();
    bool  m_ldpcFeedback   = gfsk8::ldpcFeedbackEnabled();
    int   m_maxLdpcPasses  = gfsk8::ldpcFeedbackMaxPasses();

    // Baseline polynomial coefficients.
    std::array<std::array<double, 2>,         BASELINE_N> bfitPts;
    std::array<double, BASELINE_N>            bfitCoeffs;

    float evalBaseline(float x) const {
        double b = 0.0, ex = 1.0;
        for (int i = 0; i < BASELINE_N / 2; ++i) {
            b += (bfitCoeffs[i*2] + bfitCoeffs[i*2+1] * static_cast<double>(x)) * ex;
            ex *= static_cast<double>(x) * static_cast<double>(x);
        }
        return static_cast<float>(b);
    }

    void baselinejs8(int ia, int ib)
    {
        constexpr auto bmin = static_cast<std::size_t>(
            std::round(BASELINE_MIN / Mode::DF));
        constexpr auto bmax = static_cast<std::size_t>(
            std::round(BASELINE_MAX / Mode::DF));
        constexpr auto size = bmax - bmin + 1;
        constexpr auto arm  = size / (2 * static_cast<std::size_t>(BASELINE_N));

        auto data = savg.begin() + bmin;
        auto end  = data + size;

        // Convert power → dB.
        std::transform(data, end, data,
                       [](float v){ return 10.0f * std::log10(v); });

        // Sample the lower envelope at Chebyshev nodes.
        for (int i = 0; i < BASELINE_N; ++i) {
            double const node = size * BASELINE_NODES[i];
            auto base = data + static_cast<int>(std::round(node));
            auto span = std::vector<float>(
                std::clamp(base - static_cast<std::ptrdiff_t>(arm), data, end),
                std::clamp(base + static_cast<std::ptrdiff_t>(arm), data, end));
            auto nth = span.begin() + static_cast<std::ptrdiff_t>(
                           span.size() * BASELINE_SAMPLE / 100);
            std::nth_element(span.begin(), nth, span.end());
            bfitPts[i][0] = node;
            bfitPts[i][1] = *nth;
        }

        // Solve Vandermonde system by Gaussian elimination with partial pivoting.
        std::array<std::array<double, BASELINE_N + 1>, BASELINE_N> A;
        for (int i = 0; i < BASELINE_N; ++i) {
            double xi = bfitPts[i][0];
            A[i][0] = 1.0;
            for (int j = 1; j < BASELINE_N; ++j) A[i][j] = A[i][j-1] * xi;
            A[i][BASELINE_N] = bfitPts[i][1];
        }
        for (int col = 0; col < BASELINE_N; ++col) {
            int pivot = col;
            for (int r = col+1; r < BASELINE_N; ++r)
                if (std::abs(A[r][col]) > std::abs(A[pivot][col])) pivot = r;
            std::swap(A[col], A[pivot]);
            if (std::abs(A[col][col]) < 1e-14) continue;
            double inv = 1.0 / A[col][col];
            for (int r = col+1; r < BASELINE_N; ++r) {
                double f = A[r][col] * inv;
                for (int j = col; j <= BASELINE_N; ++j) A[r][j] -= f * A[col][j];
            }
        }
        for (int r = BASELINE_N-1; r >= 0; --r) {
            if (std::abs(A[r][r]) < 1e-14) { bfitCoeffs[r] = 0.0; continue; }
            bfitCoeffs[r] = A[r][BASELINE_N];
            for (int j = r+1; j < BASELINE_N; ++j) bfitCoeffs[r] -= A[r][j] * bfitCoeffs[j];
            bfitCoeffs[r] /= A[r][r];
        }

        // Write baseline into savg[ia..ib].
        savg.fill(0.0f);
        auto const mapIdx = [ia, ib, last = static_cast<double>(size - 1)](int i) {
            return static_cast<float>((i - ia) * last / static_cast<double>(ib - ia));
        };
        for (int i = ia; i <= ib; ++i) savg[i] = evalBaseline(mapIdx(i)) + 0.65f;
    }

    void computeBasebandFFT()
    {
        float *fftw_real = reinterpret_cast<float *>(ds_cx.data());
        std::copy(dd.begin(), dd.end(), fftw_real);
        std::fill(fftw_real + dd.size(), fftw_real + Mode::NDFFT1, 0.0f);
        fftwf_execute(plans[Plan::BB]);
    }

    void js8_downsample(float f0)
    {
        constexpr float DF   = 12000.0f / Mode::NDFFT1;
        constexpr float BAUD = 12000.0f / Mode::NSPS;

        float const ft = f0 + 8.5f * BAUD;
        float const fb = f0 - 1.5f * BAUD;
        int const i0   = static_cast<int>(std::round(f0 / DF));
        int const it   = std::min(static_cast<int>(std::round(ft / DF)), Mode::NDFFT1 / 2);
        int const ib   = std::max(0, static_cast<int>(std::round(fb / DF)));

        std::size_t const NDD_SZ    = Mode::NDD + 1;
        std::size_t const RANGE_SZ  = static_cast<std::size_t>(it - ib + 1);

        std::fill_n(cd0.begin(), Mode::NDFFT2, ZERO);
        std::copy(ds_cx.begin() + ib, ds_cx.begin() + ib + RANGE_SZ, cd0.begin());

        // Apply head/tail tapers.
        auto head = cd0.begin();
        auto tail = cd0.begin() + RANGE_SZ;
        std::transform(head, head + NDD_SZ, Taper[0].begin(), head,
                       std::multiplies<>{});
        std::transform(tail - NDD_SZ, tail, Taper[1].begin(), tail - NDD_SZ,
                       std::multiplies<>{});

        // Cyclic shift to centre.
        std::rotate(cd0.begin(), cd0.begin() + (i0 - ib),
                    cd0.begin() + Mode::NDFFT2);

        fftwf_execute(plans[Plan::DS]);

        float const factor = 1.0f / std::sqrt(static_cast<float>(Mode::NDFFT1) * Mode::NDFFT2);
        std::transform(cd0.begin(), cd0.end(), cd0.begin(),
                       [factor](auto v){ return v * factor; });
    }

    float syncjs8d(int i0, float delf)
    {
        constexpr float BASE_DPHI = TAU * (1.0f / (12000.0f / Mode::NDOWN));
        std::array<std::complex<float>, Mode::NDOWNSPS> freqAdj;
        if (delf != 0.0f) {
            float dphi = BASE_DPHI * delf;
            float phi  = 0.0f;
            for (int i = 0; i < Mode::NDOWNSPS; ++i) {
                freqAdj[i] = std::polar(1.0f, phi);
                phi = std::fmod(phi + dphi, TAU);
                if (phi < 0.0f) phi += TAU;
            }
        } else {
            freqAdj.fill(std::complex<float>{1.0f, 0.0f});
        }

        float sync = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 7; ++j) {
                int const offset = 36 * i * Mode::NDOWNSPS + i0 + j * Mode::NDOWNSPS;
                if (offset >= 0 && offset + Mode::NDOWNSPS <= Mode::NP2) {
                    sync += std::norm(std::transform_reduce(
                        freqAdj.begin(), freqAdj.end(),
                        cd0.begin() + offset,
                        std::complex<float>{},
                        std::plus<>{},
                        [&](auto const &fa, auto const &cd) {
                            return cd * std::conj(fa * csyncs[i][j][&fa - &freqAdj[0]]);
                        }));
                }
            }
        return sync;
    }

    std::vector<std::complex<float>>
    genRefSig(std::array<int, NN> const &itone, float f0)
    {
        float const BFPI = TAU * f0 * (1.0f / 12000.0f);
        float phi = 0.0f;
        std::vector<std::complex<float>> cref;
        cref.reserve(static_cast<std::size_t>(NN) * Mode::NSPS);
        for (int i = 0; i < NN; ++i) {
            float const dphi = BFPI + TAU * static_cast<float>(itone[i]) / Mode::NSPS;
            for (int s = 0; s < Mode::NSPS; ++s) {
                cref.push_back(std::polar(1.0f, phi));
                phi = std::fmod(phi + dphi, TAU);
            }
        }
        return cref;
    }

    void subtractSig(std::vector<std::complex<float>> const &cref, float dt)
    {
        int const nstart = static_cast<int>(dt * 12000.0f);
        std::size_t const cref_start = (nstart < 0) ? static_cast<std::size_t>(-nstart) : 0;
        std::size_t const dd_start   = (nstart > 0) ? static_cast<std::size_t>(nstart)  : 0;
        std::size_t const sz = std::min(cref.size() - cref_start,
                                        dd.size() - dd_start);
        for (std::size_t i = 0; i < sz; ++i)
            cfilt[i] = dd[dd_start + i] * std::conj(cref[cref_start + i]);
        std::fill(cfilt.begin() + sz, cfilt.end(), ZERO);
        fftwf_execute(plans[Plan::CF]);
        std::transform(cfilt.begin(), cfilt.end(), filter.begin(),
                       cfilt.begin(), std::multiplies<>{});
        fftwf_execute(plans[Plan::CB]);
        for (std::size_t i = 0; i < sz; ++i)
            dd[dd_start + i] -= 2.0f * std::real(cfilt[i] * cref[cref_start + i]);
    }

    std::vector<SyncCandidate> syncjs8(int nfa, int nfb)
    {
        // Build symbol spectra.
        savg.fill(0.0f);
        for (int j = 0; j < Mode::NHSYM; ++j) {
            int const ia = j * Mode::NSTEP;
            int const ib = ia + Mode::NFFT1;
            if (ib > Mode::NMAX) break;
            std::transform(dd.begin() + ia, dd.begin() + ib, nuttal.begin(),
                           reinterpret_cast<float *>(sd.data()), std::multiplies<float>{});
            fftwf_execute(plans[Plan::SD]);
            for (int i = 0; i < Mode::NSPS; ++i) {
                auto const pw = std::norm(sd[i]);
                s[i][j] = pw;
                savg[i] += pw;
            }
        }

        // Sanitize filter edges.
        int const nwin = nfb - nfa;
        if (nfa < 100) { nfa = 100; if (nwin < 100) nfb = nfa + nwin; }
        if (nfb > 4910) { nfb = 4910; if (nwin < 100) nfa = nfb - nwin; }

        int const ia = std::max(0, static_cast<int>(std::round(nfa / Mode::DF)));
        int const ib = static_cast<int>(std::round(nfb / Mode::DF));

        baselinejs8(ia, ib);

        // Compute sync candidates.
        std::vector<SyncCandidate> sync;
        sync.reserve(static_cast<std::size_t>(ib - ia + 1));

        for (int i = ia; i <= ib; ++i) {
            float maxVal = -std::numeric_limits<float>::infinity();
            int   maxIdx = -Mode::JZ;

            for (int j = -Mode::JZ; j <= Mode::JZ; ++j) {
                std::array<std::array<float, 3>, 2> t{};
                for (int p = 0; p < 3; ++p)
                    for (int n = 0; n < 7; ++n) {
                        int const offset = j + Mode::JSTRT + NSSY * n + p * 36 * NSSY;
                        if (offset >= 0 && offset < Mode::NHSYM) {
                            t[0][p] += s[i + NFOS * Costas[p][n]][offset];
                            for (int f = 0; f < 7; ++f)
                                t[1][p] += s[i + NFOS * f][offset];
                        }
                    }

                auto compute = [&](int a, int b) {
                    float tx = 0.0f, t0 = 0.0f;
                    for (int k = a; k <= b; ++k) { tx += t[0][k]; t0 += t[1][k]; }
                    return tx / ((t0 - tx) / 6.0f);
                };
                float sv = std::max({compute(0,2), compute(0,1), compute(1,2)});
                if (sv > maxVal) { maxVal = sv; maxIdx = j; }
            }
            sync.push_back({Mode::DF * i, Mode::TSTEP * (maxIdx + 0.5f), maxVal});
        }

        if (sync.empty()) return {};

        // Normalise to 40th percentile.
        {
            std::vector<float> vals;
            vals.reserve(sync.size());
            for (auto const &sc : sync) vals.push_back(sc.sync);
            auto nth = vals.begin() + static_cast<std::ptrdiff_t>(vals.size() * 4 / 10);
            std::nth_element(vals.begin(), nth, vals.end());
            float const p40 = *nth;
            if (p40 != 0.0f)
                for (auto &sc : sync) sc.sync /= p40;
        }

        // Greedy candidate selection (suppress within AZ Hz).
        std::sort(sync.begin(), sync.end(),
                  [](auto const &a, auto const &b){ return a.sync > b.sync; });
        std::vector<bool> suppressed(sync.size(), false);
        std::vector<SyncCandidate> cands;
        for (std::size_t i = 0; i < sync.size() && cands.size() < NMAXCAND; ++i) {
            if (suppressed[i]) continue;
            if (sync[i].sync < ASYNCMIN || std::isnan(sync[i].sync)) break;
            cands.push_back(sync[i]);
            float const chosen = sync[i].freq;
            for (std::size_t k = i+1; k < sync.size(); ++k)
                if (std::abs(sync[k].freq - chosen) <= Mode::AZ) suppressed[k] = true;
        }
        return cands;
    }

    std::optional<DecodeResult>
    js8dec(bool syncStats, bool lsubtract,
           float &f1, float &xdt, int &nharderrors, float &xsnr,
           gfsk8inner::Event::Emitter emitEvent)
    {
        constexpr float FR  = 12000.0f / Mode::NFFT1;
        constexpr float FS2 = 12000.0f / Mode::NDOWN;
        constexpr float DT2 = 1.0f / FS2;

        int   const idx         = static_cast<int>(std::round(f1 / FR));
        float const xbase       = std::pow(10.0f, 0.1f * (savg[idx] - Mode::BASESUB));

        js8_downsample(f1);

        int   i0   = static_cast<int>(std::round((xdt + Mode::ASTART) * FS2));
        float smax = 0.0f;
        int   ibest = i0;

        for (int idt = i0 - Mode::NQSYMBOL; idt <= i0 + Mode::NQSYMBOL; ++idt) {
            float const sv = syncjs8d(idt, 0.0f);
            if (sv > smax) { smax = sv; ibest = idt; }
        }

        float const xdt2 = ibest * DT2;

        i0 = static_cast<int>(std::round(xdt2 * FS2));
        smax = 0.0f;
        float delfbest = 0.0f;
        for (int ifr = -NFSRCH; ifr <= NFSRCH; ++ifr) {
            float const delf = ifr * 0.5f;
            float const sv   = syncjs8d(i0, delf);
            if (sv > smax) { smax = sv; delfbest = delf; }
        }

        // Apply fine frequency correction.
        float const dphi = -delfbest * (TAU / FS2);
        auto const wstep = std::polar(1.0f, dphi);
        auto w = std::complex<float>{1.0f, 0.0f};
        for (int i = 0; i < Mode::NP2; ++i) { w *= wstep; cd0[i] *= w; }
        xdt = xdt2;
        f1 += delfbest;

        float const sync = syncjs8d(i0, 0.0f);

        std::array<std::array<float, NN>, NROWS> s2;

        // Set up frequency and timing trackers.
        gfsk8::FrequencyTracker freqTracker;
        if (m_freqTracking) freqTracker.reset(0.0, FS2);
        else                freqTracker.disable();

        double const timingMaxShift = std::clamp(0.08 * Mode::NDOWNSPS, 0.5, 2.0);
        gfsk8::TimingTracker timingTracker;
        if (m_timingTracking) timingTracker.reset(0.0, 0.15, 0.35, timingMaxShift);
        else                  timingTracker.disable();

        // Per-symbol parabolic peak interpolation for frequency residual.
        auto estimateResidual = [&](int expTone) -> std::optional<float> {
            if (!freqTracker.enabled() || expTone < 0 ||
                expTone + 1 >= Mode::NDOWNSPS) return std::nullopt;
            float const m0  = std::norm(csymb[expTone]);
            float const mp  = std::norm(csymb[expTone + 1]);
            float const mm  = (expTone > 0) ? std::norm(csymb[expTone - 1]) : 0.0f;
            if (m0 <= 0.0f || m0 / (mp + mm + 1e-12f) < 1.5f) return std::nullopt;
            float const den = mm - 2.0f * m0 + mp;
            if (std::abs(den) < 1e-9f) return std::nullopt;
            float delta = std::clamp(0.5f * (mm - mp) / den, -0.5f, 0.5f);
            return delta * (FS2 / Mode::NDOWNSPS);
        };

        // Goertzel-style single-bin energy for timing feedback.
        auto goertzelEnergy = [&](int start, int expTone) -> std::optional<float> {
            if (start < 0 || start + Mode::NDOWNSPS > Mode::NP2) return std::nullopt;
            std::array<std::complex<float>, Mode::NDOWNSPS> tmp;
            std::copy(cd0.begin() + start, cd0.begin() + start + Mode::NDOWNSPS, tmp.begin());
            if (freqTracker.enabled()) freqTracker.apply(tmp.data(), Mode::NDOWNSPS);
            auto const ws = std::polar(1.0f, static_cast<float>(-TAU * expTone / Mode::NDOWNSPS));
            auto ph = std::complex<float>{1.0f, 0.0f};
            std::complex<float> acc{};
            for (auto const &x : tmp) { acc += x * std::conj(ph); ph *= ws; }
            return std::norm(acc);
        };

        // Process all 79 symbols.
        for (int k = 0; k < NN; ++k) {
            int const i1base = ibest + k * Mode::NDOWNSPS;
            int const tshift = timingTracker.enabled()
                               ? static_cast<int>(std::round(timingTracker.currentSamples()))
                               : 0;
            int i1 = std::clamp(i1base + tshift, 0, Mode::NP2 - Mode::NDOWNSPS);

            csymb.fill(ZERO);
            if (i1 >= 0 && i1 + Mode::NDOWNSPS <= Mode::NP2) {
                std::copy(cd0.begin() + i1, cd0.begin() + i1 + Mode::NDOWNSPS, csymb.begin());
                if (freqTracker.enabled()) freqTracker.apply(csymb.data(), Mode::NDOWNSPS);
            }
            fftwf_execute(plans[Plan::CS]);
            for (int i = 0; i < NROWS; ++i) s2[i][k] = std::abs(csymb[i]) / 1000.0f;

            // Update trackers from Costas pilot symbols.
            bool const isPilot = (k < 7) || (k >= 36 && k < 43) || (k >= 72 && k < 79);
            if (isPilot && (freqTracker.enabled() || timingTracker.enabled())) {
                int cblock = 0, ccol = k;
                if (k >= 36 && k < 43) { cblock = 1; ccol = k - 36; }
                else if (k >= 72)      { cblock = 2; ccol = k - 72; }
                int const expTone = Costas[cblock][ccol];

                if (auto res = estimateResidual(expTone)) freqTracker.update(*res);

                if (timingTracker.enabled()) {
                    auto const e0  = goertzelEnergy(i1,     expTone);
                    auto const ee  = goertzelEnergy(i1 - 1, expTone);
                    auto const el  = goertzelEnergy(i1 + 1, expTone);
                    float const tm = s2[expTone][k];
                    if (e0 && ee && el && tm > 1e-6f) {
                        float const grad = (*el - *ee) / (*e0 + 1e-6f);
                        double const w   = std::clamp(static_cast<double>(tm / 5.0f), 0.0, 1.0);
                        double const err = std::clamp(0.25 * static_cast<double>(grad), -1.0, 1.0);
                        timingTracker.update(err, w);
                    }
                }
            }
        }

        // Validate sync quality (≥ 7 Costas matches required).
        int nsync = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            int const off = static_cast<int>(c) * 36;
            for (int col = 0; col < 7; ++col) {
                auto maxRow = static_cast<int>(std::distance(
                    s2.begin(),
                    std::max_element(s2.begin(), s2.end(),
                        [idx2 = off + col](auto const &ra, auto const &rb){
                            return ra[idx2] < rb[idx2]; })));
                if (Costas[c][col] == maxRow) ++nsync;
            }
        }
        if (nsync <= 6) return std::nullopt;

        if (syncStats)
            emitEvent(gfsk8inner::Event::SyncState{
                gfsk8inner::Event::SyncState::Kind::CANDIDATE,
                Mode::NSUBMODE, f1, xdt, {.candidate = nsync}});

        // Extract data symbol magnitudes (strip Costas).
        std::array<std::array<float, ND>, NROWS> s1;
        for (int row = 0; row < NROWS; ++row) {
            std::copy(s2[row].begin() + 7,  s2[row].begin() + 36, s1[row].begin());
            std::copy(s2[row].begin() + 43, s2[row].begin() + 72, s1[row].begin() + 29);
        }

        // Winner selection.
        std::array<int, ND> winners{};
        for (int j = 0; j < ND; ++j) {
            int best = 0;
            for (int i = 1; i < NROWS; ++i)
                if (s1[i][j] > s1[best][j]) best = i;
            winners[j] = best;
        }

        auto wh = gfsk8::WhiteningProcessor<NROWS, ND, N>::process(
            s1, winners, m_erasureThresh, false);
        auto llr0 = wh.llr0;
        auto llr1 = wh.llr1;

        // Apply erasure threshold if whitening didn't do it.
        if (!wh.erasureApplied && m_erasureThresh > 0.0f) {
            for (auto &v : llr0) if (std::abs(v) < m_erasureThresh) v = 0.0f;
            for (auto &v : llr1) if (std::abs(v) < m_erasureThresh) v = 0.0f;
        }

        // Soft combining.
        auto const ttl = std::chrono::seconds{Mode::NTXDUR * 2};
        softCombiner.flush(ttl);
        auto const ckey     = softCombiner.makeKey(Mode::NSUBMODE, f1, xdt, llr0, llr1);
        auto       combined = softCombiner.combine(ckey, llr0, llr1, ttl);
        auto llr0c = combined.llr0;
        auto llr1c = combined.llr1;

        std::array<int8_t, K> decoded;
        std::array<int8_t, N> cw;
        int totalPasses = 0;

        auto tryDecode = [&](std::array<float, N> const &llrInput, int ipass)
            -> std::optional<DecodeResult>
        {
            nharderrors = bpdecode174(llrInput, decoded, cw);
            xsnr = -99.0f;
            if (std::all_of(cw.begin(), cw.end(), [](int x){ return x == 0; }))
                return std::nullopt;
            if (nharderrors >= 0 && nharderrors < 60 &&
                !(sync < 2.0f && nharderrors > 35) &&
                !(ipass > 2   && nharderrors > 39) &&
                !(ipass == 4  && nharderrors > 30))
            {
                if (checkCRC12(decoded)) {
                    if (syncStats)
                        emitEvent(gfsk8inner::Event::SyncState{
                            gfsk8inner::Event::SyncState::Kind::DECODED,
                            Mode::NSUBMODE, f1, xdt2, {.decoded = sync}});

                    auto msg = extractMessage(decoded);
                    if (msg.empty()) return std::nullopt;

                    int const i3bit = (decoded[72] << 2) | (decoded[73] << 1) | decoded[74];
                    std::array<int, NN> itone;
                    gfsk8inner::encode(i3bit, Costas, msg.data(), itone.data());

                    if (lsubtract) subtractSig(genRefSig(itone, f1), xdt2);

                    float xsig = 0.0f;
                    for (int ii = 0; ii < NN; ++ii) xsig += std::pow(s2[itone[ii]][ii], 2);
                    xsnr = std::max(10.0f * std::log10(
                               std::max(xsig / xbase - 1.0f, 1.259e-10f)) - 32.0f, -60.0f);

                    softCombiner.markDecoded(combined.key);
                    return DecodeResult{i3bit, msg};
                }
            } else {
                nharderrors = -1;
            }
            return std::nullopt;
        };

        for (int ipass = 1; ipass <= 4 && totalPasses < m_maxLdpcPasses; ++ipass) {
            auto &llrRef = (ipass == 2) ? llr1c : llr0c;
            if (ipass == 3) std::fill(llr0c.begin(), llr0c.begin() + 24, 0.0f);
            if (ipass == 4) std::fill(llr0c.begin() + 24, llr0c.begin() + 48, 0.0f);

            std::array<float, N> llrPrim = llrRef;
            if (auto r = tryDecode(llrPrim, ipass)) { ++totalPasses; return r; }
            ++totalPasses;

            if (m_ldpcFeedback && totalPasses < m_maxLdpcPasses) {
                std::array<float, N> llrRef2;
                int conf = 0, unc = 0;
                gfsk8::refineLlrsWithLdpcFeedback(llrPrim, cw, m_erasureThresh,
                                                llrRef2, conf, unc);
                if (auto r = tryDecode(llrRef2, ipass)) { ++totalPasses; return r; }
                ++totalPasses;
            }
        }
        return std::nullopt;
    }

public:
    DecodeEngine()
    {
        // Nuttall window (Kahan summation for Fortran parity).
        float const pi = 4.0f * std::atan(1.0f);
        constexpr float a0 = 0.3635819f, a1 = -0.4891775f,
                        a2 = 0.1365995f, a3 = -0.0106411f;
        float wsum = 0.0f;
        for (std::size_t i = 0; i < nuttal.size(); ++i) {
            KahanSum<float> v = a0;
            v += a1 * std::cos(2 * pi * i / nuttal.size());
            v += a2 * std::cos(4 * pi * i / nuttal.size());
            v += a3 * std::cos(6 * pi * i / nuttal.size());
            nuttal[i] = v;
            wsum += static_cast<float>(v);
        }
        for (auto &v : nuttal) v = v / wsum * nuttal.size() / 300.0f;

        // Costas complex waveforms.
        for (int i = 0; i < 7; ++i) {
            float pa = 0.0f, pb = 0.0f, pc = 0.0f;
            float const da = TAU * Costas[0][i] / Mode::NDOWNSPS;
            float const db = TAU * Costas[1][i] / Mode::NDOWNSPS;
            float const dc = TAU * Costas[2][i] / Mode::NDOWNSPS;
            for (int j = 0; j < Mode::NDOWNSPS; ++j) {
                csyncs[0][i][j] = std::polar(1.0f, pa);
                csyncs[1][i][j] = std::polar(1.0f, pb);
                csyncs[2][i][j] = std::polar(1.0f, pc);
                pa = std::fmod(pa + da, TAU);
                pb = std::fmod(pb + db, TAU);
                pc = std::fmod(pc + dc, TAU);
            }
        }

        // Hann-window LPF for signal subtraction.
        float fsum = 0.0f;
        for (int j = -NFILT/2; j <= NFILT/2; ++j) {
            float const v = std::pow(std::cos(pi * j / NFILT), 2);
            filter[j + NFILT/2].real(v);
            fsum += v;
        }
        std::fill(std::transform(filter.begin(), filter.begin() + NFILT + 1,
                                 filter.begin(),
                                 [fsum](auto v){ return std::complex<float>(v.real()/fsum, 0.0f); }),
                  filter.end(), ZERO);
        std::rotate(filter.begin(), filter.begin() + NFILT/2, filter.begin() + NFILT + 1);

        // FFT-transform the filter kernel.
        {
            fftwf_plan fp;
            {
                std::lock_guard<std::mutex> lk(g_fftMutex);
                fp = fftwf_plan_dft_1d(
                    Mode::NMAX,
                    reinterpret_cast<fftwf_complex *>(filter.data()),
                    reinterpret_cast<fftwf_complex *>(filter.data()),
                    FFTW_FORWARD, FFTW_ESTIMATE_PATIENT);
                if (!fp) throw std::runtime_error("FFT plan creation failed");
            }
            fftwf_execute(fp);
            { std::lock_guard<std::mutex> lk(g_fftMutex); fftwf_destroy_plan(fp); }
        }
        float const invN = 1.0f / Mode::NMAX;
        std::transform(filter.begin(), filter.end(), filter.begin(),
                       [invN](auto v){ return v * invN; });

        // Create permanent FFT plans.
        std::lock_guard<std::mutex> lk(g_fftMutex);
        plans[Plan::DS] = fftwf_plan_dft_1d(
            Mode::NDFFT2,
            reinterpret_cast<fftwf_complex *>(cd0.data()),
            reinterpret_cast<fftwf_complex *>(cd0.data()),
            FFTW_BACKWARD, FFTW_ESTIMATE_PATIENT);
        plans[Plan::BB] = fftwf_plan_dft_r2c_1d(
            Mode::NDFFT1,
            reinterpret_cast<float *>(ds_cx.data()),
            reinterpret_cast<fftwf_complex *>(ds_cx.data()),
            FFTW_ESTIMATE_PATIENT);
        plans[Plan::CF] = fftwf_plan_dft_1d(
            Mode::NMAX,
            reinterpret_cast<fftwf_complex *>(cfilt.data()),
            reinterpret_cast<fftwf_complex *>(cfilt.data()),
            FFTW_FORWARD, FFTW_ESTIMATE_PATIENT);
        plans[Plan::CB] = fftwf_plan_dft_1d(
            Mode::NMAX,
            reinterpret_cast<fftwf_complex *>(cfilt.data()),
            reinterpret_cast<fftwf_complex *>(cfilt.data()),
            FFTW_BACKWARD, FFTW_ESTIMATE_PATIENT);
        plans[Plan::SD] = fftwf_plan_dft_r2c_1d(
            Mode::NFFT1,
            reinterpret_cast<float *>(sd.data()),
            reinterpret_cast<fftwf_complex *>(sd.data()),
            FFTW_ESTIMATE_PATIENT);
        plans[Plan::CS] = fftwf_plan_dft_1d(
            Mode::NDOWNSPS,
            reinterpret_cast<fftwf_complex *>(csymb.data()),
            reinterpret_cast<fftwf_complex *>(csymb.data()),
            FFTW_FORWARD, FFTW_ESTIMATE_PATIENT);
        for (auto p : plans)
            if (!p) throw std::runtime_error("FFT plan creation failed");
    }

    std::size_t operator()(DecodeData const &data, int kpos, int ksz,
                           gfsk8inner::Event::Emitter emitEvent)
    {
        int const pos = std::max(0, kpos);
        int const sz  = std::max(0, ksz);
        assert(sz <= Mode::NMAX);

        if (data.params.syncStats)
            emitEvent(gfsk8inner::Event::SyncStart{pos, sz});

        // Copy audio into dd[].
        dd.fill(0.0f);
        if ((GFSK8_RX_SAMPLE_SIZE - pos) < sz) {
            int const first  = GFSK8_RX_SAMPLE_SIZE - pos;
            int const second = sz - first;
            std::transform(data.d2 + pos, data.d2 + pos + first,   dd.begin(),
                           [](auto v){ return static_cast<float>(v); });
            std::transform(data.d2,       data.d2 + second,         dd.begin() + first,
                           [](auto v){ return static_cast<float>(v); });
        } else {
            std::transform(data.d2 + pos, data.d2 + pos + sz, dd.begin(),
                           [](auto v){ return static_cast<float>(v); });
        }

        DecodeResult::Map decodes;
        auto const ttl = std::chrono::seconds{Mode::NTXDUR * 2};
        softCombiner.flush(ttl);

        for (int ipass = 1; ipass <= 3; ++ipass) {
            auto cands = syncjs8(data.params.nfa, data.params.nfb);
            if (cands.empty()) break;

            // Sort: nfqso hits first, then by frequency proximity.
            std::sort(cands.begin(), cands.end(),
                [nfqso = data.params.nfqso](auto const &a, auto const &b) {
                    float da = std::abs(a.freq - nfqso);
                    float db = std::abs(b.freq - nfqso);
                    if (da < 10.0f && db >= 10.0f) return true;
                    if (db < 10.0f && da >= 10.0f) return false;
                    return std::tie(da, a.freq) < std::tie(db, b.freq);
                });

            computeBasebandFFT();
            bool improved = false;

            for (auto [f1, xdt, syncV] : cands) {
                float xsnr = 0.0f;
                int nerr   = -1;
                if (auto dec = js8dec(data.params.syncStats, ipass < 3,
                                      f1, xdt, nerr, xsnr, emitEvent))
                {
                    int const snr = static_cast<int>(std::round(xsnr));
                    if (auto [it, ins] = decodes.try_emplace(std::move(*dec), snr);
                        ins || it->second < snr)
                    {
                        improved   = true;
                        if (!ins) it->second = snr;
                        emitEvent(gfsk8inner::Event::Decoded{
                            data.params.nutc, snr, xdt - Mode::ASTART, f1,
                            it->first.data, it->first.type,
                            1.0f - nerr / 60.0f, Mode::NSUBMODE});
                    }
                }
            }
            if (!improved) break;
        }

        return decodes.size();
    }
};

// Explicit instantiations.
template class DecodeEngine<ModeA>;
template class DecodeEngine<ModeB>;
template class DecodeEngine<ModeC>;
template class DecodeEngine<ModeE>;
template class DecodeEngine<ModeI>;

} // anonymous namespace

// ── Decoder::Impl ─────────────────────────────────────────────────────────────

struct gfsk8inner::Decoder::Impl {
    DecodeData &m_data;

    struct Entry {
        std::variant<DecodeEngine<ModeA>, DecodeEngine<ModeB>,
                     DecodeEngine<ModeC>, DecodeEngine<ModeE>,
                     DecodeEngine<ModeI>> engine;
        int  mode;
        int &kpos;
        int &ksz;

        template <typename E>
        Entry(std::in_place_type_t<E>, int m, int &kp, int &ks)
            : engine(std::in_place_type<E>), mode(m), kpos(kp), ksz(ks) {}
    };

    template <typename E>
    Entry makeEntry(int shift, int &kpos, int &ksz) {
        return Entry(std::in_place_type<E>, 1 << shift, kpos, ksz);
    }

    std::array<Entry, 5> entries = {{
        makeEntry<DecodeEngine<ModeI>>(4, m_data.params.kposI, m_data.params.kszI),
        makeEntry<DecodeEngine<ModeE>>(3, m_data.params.kposE, m_data.params.kszE),
        makeEntry<DecodeEngine<ModeC>>(2, m_data.params.kposC, m_data.params.kszC),
        makeEntry<DecodeEngine<ModeB>>(1, m_data.params.kposB, m_data.params.kszB),
        makeEntry<DecodeEngine<ModeA>>(0, m_data.params.kposA, m_data.params.kszA),
    }};

    explicit Impl(DecodeData &data) : m_data(data) {}

    void run(Event::Emitter const &emit) {
        int const mask = m_data.params.nsubmodes;
        std::size_t total = 0;
        emit(Event::DecodeStarted{mask});
        for (auto &e : entries) {
            if ((mask & e.mode) == e.mode) {
                std::visit([&](auto &eng) {
                    total += eng(m_data, e.kpos, e.ksz, emit);
                }, e.engine);
            }
        }
        emit(Event::DecodeFinished{total});
    }
};

gfsk8inner::Decoder::Decoder()
    : m_impl(std::make_unique<Impl>(m_snapshot)) {}

gfsk8inner::Decoder::~Decoder() = default;

void gfsk8inner::Decoder::snapshot() { m_snapshot = g_decodeData; }

void gfsk8inner::Decoder::decode(Event::Emitter const &emit) { m_impl->run(emit); }

// ── Encoder ───────────────────────────────────────────────────────────────────

void gfsk8inner::encode(int frameType, Costas::Array const &costas,
                      const char *message, int *tones)
{
    // Pack 12 × 6-bit characters into 9 bytes (72 bits).
    std::array<uint8_t, 11> bytes{};
    for (int i = 0, j = 0; i < 12; i += 4, j += 3) {
        uint32_t words = (static_cast<uint32_t>(alphabetWord(message[i]))   << 18) |
                         (static_cast<uint32_t>(alphabetWord(message[i+1])) << 12) |
                         (static_cast<uint32_t>(alphabetWord(message[i+2])) <<  6) |
                          static_cast<uint32_t>(alphabetWord(message[i+3]));
        bytes[j]   = static_cast<uint8_t>(words >> 16);
        bytes[j+1] = static_cast<uint8_t>(words >>  8);
        bytes[j+2] = static_cast<uint8_t>(words);
    }

    // Append 3-bit frame type at bits 72–74.
    bytes[9] = static_cast<uint8_t>((frameType & 0x7) << 5);

    // CRC-12 occupies bits 75–86.
    uint16_t const crc = CRC12(bytes);
    bytes[9]  |= static_cast<uint8_t>((crc >> 7) & 0x1Fu);
    bytes[10]  = static_cast<uint8_t>((crc & 0x7Fu) << 1);

    // Write Costas arrays at tone positions 0, 36, 72.
    auto *costasPtr = tones;
    for (auto const &row : costas) {
        std::copy(row.begin(), row.end(), costasPtr);
        costasPtr += 36;
    }

    // Generate parity and data tones in parallel (29 × 3-bit symbols each).
    int *parityPtr = tones + 7;
    int *dataPtr   = tones + 43;

    std::size_t outBits = 0, outByte = 0;
    uint8_t outMask = 0x80, outWord = 0, parWord = 0;

    for (std::size_t i = 0; i < 87; ++i) {
        // Accumulate parity bit.
        std::size_t parBits = 0, parByte = 0;
        uint8_t parMask = 0x80;
        for (std::size_t j = 0; j < 87; ++j) {
            parBits += static_cast<std::size_t>(parity(i, j)) &
                       static_cast<std::size_t>((bytes[parByte] & parMask) != 0);
            parMask = (parMask == 1) ? (++parByte, uint8_t{0x80}) : (parMask >> 1);
        }

        parWord = static_cast<uint8_t>((parWord << 1) | (parBits & 1u));
        outWord = static_cast<uint8_t>((outWord << 1) | ((bytes[outByte] & outMask) != 0));
        outMask = (outMask == 1) ? (++outByte, uint8_t{0x80}) : (outMask >> 1);

        if (++outBits == 3) {
            *parityPtr++ = parWord;
            *dataPtr++   = outWord;
            parWord = outWord = 0;
            outBits = 0;
        }
    }
}
