/**
 * @file JSC.cpp
 * @brief JSC word-based compression implementation.
 *
 * Rewritten from the original JSC.cpp.  The algorithms and all numeric
 * constants are identical; the rewrite uses renamed local variables and
 * a different internal cache name (s_wordCache instead of LOOKUP_CACHE).
 *
 * (C) 2018 Jordan Sherer <kn4crd@gmail.com> - All Rights Reserved
 */

#include "JSC.h"
#include "Varicode.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

// Per-process cache: maps word string → table index.
static std::unordered_map<std::string, uint32_t> s_wordCache;

// ── Codeword generation ───────────────────────────────────────────────────────

Codeword JSC::codeword(uint32_t index, bool separate, uint32_t bytesize,
                       uint32_t s, uint32_t c)
{
    std::vector<Codeword> segments;

    // Leaf segment: low-order bits of (index % s), plus the separator flag.
    uint32_t v = ((index % s) << 1) | static_cast<uint32_t>(separate);
    segments.insert(segments.begin(), Varicode::intToBits(v, bytesize + 1));

    // Extension segments: one per level above the base.
    uint32_t x = index / s;
    while (x > 0) {
        x -= 1;
        segments.insert(segments.begin(),
                        Varicode::intToBits((x % c) + s, bytesize));
        x /= c;
    }

    // Flatten segments into a single bit vector.
    Codeword word;
    for (auto const &seg : segments)
        word.insert(word.end(), seg.begin(), seg.end());

    return word;
}

// ── Compression ───────────────────────────────────────────────────────────────

std::vector<CodewordPair> JSC::compress(std::string const &text)
{
    std::vector<CodewordPair> result;

    constexpr uint32_t b = 4;
    constexpr uint32_t s = 7;
    const     uint32_t c = static_cast<uint32_t>(std::pow(2, 4)) - s;

    // Split on spaces, preserving empty tokens between consecutive spaces.
    std::vector<std::string> words;
    {
        std::string cur;
        for (char ch : text) {
            if (ch == ' ') { words.push_back(cur); cur.clear(); }
            else           { cur += ch; }
        }
        words.push_back(cur);
    }

    for (int i = 0, total = static_cast<int>(words.size()); i < total; ++i) {
        std::string w = words[i];
        bool const isLastWord      = (i == total - 1);
        bool       isSpaceChar     = false;

        // An empty intermediate token represents a literal space character.
        if (w.empty() && !isLastWord) {
            w = " ";
            isSpaceChar = true;
        }

        while (!w.empty()) {
            bool found = false;
            auto const idx = lookup(w, &found);
            if (!found) break;

            auto const &entry = JSC::map[idx];
            w = w.substr(static_cast<std::size_t>(entry.size));

            bool const isLast           = w.empty();
            bool const appendTrailSpace = isLast && !isSpaceChar && !isLastWord;

            result.push_back({
                codeword(idx, appendTrailSpace, b, s, c),
                static_cast<uint32_t>(entry.size) + (appendTrailSpace ? 1u : 0u)
            });
        }
    }

    return result;
}

// ── Decompression ─────────────────────────────────────────────────────────────

std::string JSC::decompress(Codeword const &bitvec)
{
    constexpr uint32_t b = 4;
    constexpr uint32_t s = 7;
    const     uint32_t c = static_cast<uint32_t>(std::pow(2, b)) - s;

    // Pre-compute base offsets for each encoding depth (0..7).
    uint32_t base[8];
    base[0] = 0;
    base[1] = s;
    base[2] = base[1] + s * c;
    base[3] = base[2] + s * c * c;
    base[4] = base[3] + s * c * c * c;
    base[5] = base[4] + s * c * c * c * c;
    base[6] = base[5] + s * c * c * c * c * c;
    base[7] = base[6] + s * c * c * c * c * c * c;

    // Decode the bit vector into a list of 4-bit bytes plus separator positions.
    std::vector<uint64_t> nibbles;
    std::vector<uint32_t> separatorPositions;

    int pos   = 0;
    int total = static_cast<int>(bitvec.size());
    while (pos < total) {
        Codeword chunk(bitvec.begin() + pos,
                       bitvec.begin() + std::min(pos + 4, total));
        if (static_cast<int>(chunk.size()) != 4) break;

        uint64_t nibble = Varicode::bitsToInt(chunk);
        nibbles.push_back(nibble);
        pos += 4;

        if (nibble < s) {
            if (total - pos > 0 && bitvec[pos])
                separatorPositions.push_back(
                    static_cast<uint32_t>(nibbles.size()) - 1);
            pos += 1;
        }
    }

    // Reconstruct words from the nibble stream.
    std::vector<std::string> parts;
    uint32_t start = 0;
    while (start < static_cast<uint32_t>(nibbles.size())) {
        uint32_t depth = 0;
        uint32_t accum = 0;

        while (start + depth < static_cast<uint32_t>(nibbles.size()) &&
               nibbles[start + depth] >= s)
        {
            accum = accum * c + static_cast<uint32_t>(nibbles[start + depth] - s);
            ++depth;
        }
        if (accum >= JSC::size) break;
        if (start + depth >= static_cast<uint32_t>(nibbles.size())) break;

        uint32_t idx = accum * s +
                       static_cast<uint32_t>(nibbles[start + depth]) +
                       base[depth];
        if (idx >= JSC::size) break;

        parts.push_back(std::string(JSC::map[idx].str));

        if (!separatorPositions.empty() &&
            separatorPositions.front() == start + depth)
        {
            parts.push_back(" ");
            separatorPositions.erase(separatorPositions.begin());
        }

        start += depth + 1;
    }

    std::string out;
    for (auto const &p : parts) out += p;
    return out;
}

// ── Existence check ───────────────────────────────────────────────────────────

bool JSC::exists(std::string const &w, uint32_t *pIndex)
{
    bool     found = false;
    uint32_t idx   = lookup(w, &found);
    if (pIndex) *pIndex = idx;
    return found && JSC::map[idx].size == static_cast<int>(w.length());
}

// ── Lookup (std::string overload) ─────────────────────────────────────────────

uint32_t JSC::lookup(std::string const &w, bool *ok)
{
    auto it = s_wordCache.find(w);
    if (it != s_wordCache.end()) {
        if (ok) *ok = true;
        return it->second;
    }

    bool     found = false;
    uint32_t idx   = lookup(w.c_str(), &found);
    if (found) s_wordCache[w] = idx;

    if (ok) *ok = found;
    return idx;
}

// ── Lookup (C-string overload) ────────────────────────────────────────────────

uint32_t JSC::lookup(char const *b, bool *ok)
{
    uint32_t startIdx = 0;
    uint32_t rangeLen = 0;
    bool     havePfx  = false;

    // Find the prefix table entry whose first character matches.
    for (uint32_t i = 0; i < JSC::prefixSize; ++i) {
        if (b[0] != JSC::prefix[i].str[0]) continue;

        // Single-character match: return immediately.
        if (JSC::prefix[i].size == 1) {
            if (ok) *ok = true;
            return JSC::list[JSC::prefix[i].index].index;
        }

        startIdx = JSC::prefix[i].index;
        rangeLen = JSC::prefix[i].size;
        havePfx  = true;
        break;
    }

    if (!havePfx) {
        if (ok) *ok = false;
        return 0;
    }

    // Linear scan through the prefix's range in the list table.
    for (uint32_t i = startIdx; i < startIdx + rangeLen; ++i) {
        uint32_t len = static_cast<uint32_t>(JSC::list[i].size);
        if (std::strncmp(b, JSC::list[i].str, len) == 0) {
            if (ok) *ok = true;
            return JSC::list[i].index;
        }
    }

    if (ok) *ok = false;
    return 0;
}
