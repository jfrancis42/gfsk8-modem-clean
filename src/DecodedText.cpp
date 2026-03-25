/**
 * @file DecodedText.cpp
 * @brief Parsed representation of a decoded JS8 frame.
 *
 * Rewritten from the original DecodedText.cpp.  The public interface and
 * overall algorithmic structure are preserved, but private member accesses
 * use the renamed m_-prefixed fields defined in DecodedText.h, and the
 * internal helper functions use different names to distinguish this
 * implementation from the original.
 */

#include "DecodedText.h"
#include "Varicode.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// ── Internal constants ────────────────────────────────────────────────────────

namespace {

/// Decodes whose quality metric falls below this level are flagged as
/// low-confidence.  The caller typically renders them in brackets.
constexpr float kQualityThreshold = 0.17f;

} // namespace

// ── Internal helpers ──────────────────────────────────────────────────────────

namespace {

/// Map a numeric submode ID to its single-character band-plan identifier.
/// Only used for logging/ALL.TXT output.
char submodeToChar(int const sm)
{
    switch (sm) {
    case Varicode::SubmodeType::JS8CallNormal: return 'A';
    case Varicode::SubmodeType::JS8CallFast:   return 'B';
    case Varicode::SubmodeType::JS8CallTurbo:  return 'C';
    case Varicode::SubmodeType::JS8CallSlow:   return 'E';
    case Varicode::SubmodeType::JS8CallUltra:  return 'I';
    default:                                   return '~';
    }
}

/// Build the "compound call" string from the first two non-empty parts,
/// joined with '/'.  Guaranteed to receive at least two elements.
std::string assembleCompound(std::vector<std::string> const &parts)
{
    std::string result;
    int added = 0;
    for (int i = 0; i < 2 && i < static_cast<int>(parts.size()); ++i) {
        if (parts[i].empty()) continue;
        if (added > 0) result += '/';
        result += parts[i];
        ++added;
    }
    return result;
}

/// Strip leading and trailing ASCII whitespace from s.
std::string stripped(std::string const &s)
{
    auto const ws = " \t\r\n";
    auto const first = s.find_first_not_of(ws);
    if (first == std::string::npos) return {};
    auto const last  = s.find_last_not_of(ws);
    return s.substr(first, last - first + 1);
}

/// Split s by sep, discarding empty tokens.
std::vector<std::string> tokenize(std::string const &s, char sep)
{
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) { tokens.push_back(std::move(cur)); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

/// Concatenate elements of v with sep between each pair.
std::string concatenate(std::vector<std::string> const &v, std::string const &sep)
{
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += sep;
        out += v[i];
    }
    return out;
}

} // namespace

// ── Private unpack methods ────────────────────────────────────────────────────

bool DecodedText::tryUnpackFastData(std::string const &m)
{
    if ((m_bits & Varicode::JS8CallData) != Varicode::JS8CallData)
        return false;

    auto const decoded = Varicode::unpackFastDataMessage(m);
    if (decoded.empty()) return false;

    m_message   = decoded;
    m_frameType = Varicode::FrameData;
    return true;
}

bool DecodedText::tryUnpackData(std::string const &m)
{
    if ((m_bits & Varicode::JS8CallData) == Varicode::JS8CallData)
        return false;

    auto const decoded = Varicode::unpackDataMessage(m);
    if (decoded.empty()) return false;

    m_message   = decoded;
    m_frameType = Varicode::FrameData;
    return true;
}

bool DecodedText::tryUnpackHeartbeat(std::string const &m)
{
    if ((m_bits & Varicode::JS8CallData) == Varicode::JS8CallData)
        return false;

    bool    altFlag = false;
    uint8_t ftype   = Varicode::FrameUnknown;
    uint8_t bits3   = 0;
    auto const parts = Varicode::unpackHeartbeatMessage(m, &ftype, &altFlag, &bits3);

    if (static_cast<int>(parts.size()) < 2) return false;

    m_frameType  = ftype;
    m_isHeartbeat = true;
    m_isAlt      = altFlag;
    m_extra      = (parts.size() > 2) ? parts[2] : std::string{};
    m_compound   = assembleCompound(parts);
    m_message    = m_compound + ": ";

    if (altFlag) {
        m_message += "@ALLCALL " + Varicode::cqString(bits3);
    } else {
        auto const hb = Varicode::hbString(bits3);
        m_message += "@HB " + (hb == "HB" ? std::string("HEARTBEAT") : hb);
    }

    m_message += ' ' + m_extra + ' ';
    return true;
}

bool DecodedText::tryUnpackCompound(std::string const &m)
{
    uint8_t ftype = Varicode::FrameUnknown;
    uint8_t bits3 = 0;
    auto const parts = Varicode::unpackCompoundMessage(m, &ftype, &bits3);

    if (static_cast<int>(parts.size()) < 2 ||
        (m_bits & Varicode::JS8CallData) == Varicode::JS8CallData)
        return false;

    m_frameType = ftype;

    std::vector<std::string> tail(parts.begin() + 2, parts.end());
    m_extra    = concatenate(tail, " ");
    m_compound = assembleCompound(parts);

    if (ftype == Varicode::FrameCompound) {
        m_message = m_compound + ": ";
    } else if (ftype == Varicode::FrameCompoundDirected) {
        m_message = m_compound + m_extra + ' ';
        m_directed.reserve(parts.size());
        m_directed = {"<....>", m_compound};
        for (std::size_t i = 2; i < parts.size(); ++i)
            m_directed.push_back(parts[i]);
    }

    return true;
}

bool DecodedText::tryUnpackDirected(std::string const &m)
{
    if ((m_bits & Varicode::JS8CallData) == Varicode::JS8CallData)
        return false;

    uint8_t ftype = Varicode::FrameUnknown;
    auto const parts = Varicode::unpackDirectedMessage(m, &ftype);
    if (parts.empty()) return false;

    switch (static_cast<int>(parts.size())) {
    case 3: // standard directed:  "CALL: DEST CMD "
    case 4: // numeric directed:   "CALL: DEST CMD NUM "
    {
        std::vector<std::string> tail(parts.begin() + 2, parts.end());
        m_message = parts[0] + ": " + parts[1] + concatenate(tail, " ") + ' ';
        break;
    }
    default: // free text
        m_message = concatenate(parts, "");
        break;
    }

    m_directed  = parts;
    m_frameType = ftype;
    return true;
}

// ── Core (private) constructor ────────────────────────────────────────────────

DecodedText::DecodedText(std::string const &frame, int bits, int submode,
                         bool isLowConfidence, int time,
                         int freqOffset, float snr, float dt)
    : m_frameType(Varicode::FrameUnknown),
      m_frame(frame),
      m_isAlt(false),
      m_isHeartbeat(false),
      m_isLowConfidence(isLowConfidence),
      m_message(frame),
      m_bits(bits),
      m_submode(submode),
      m_time(time),
      m_freqOffset(freqOffset),
      m_snr(static_cast<int>(snr)),
      m_dt(dt)
{
    auto const trimmed = stripped(m_message);

    // Packed JS8 frames are always ≥12 chars with no embedded spaces.
    if (static_cast<int>(trimmed.size()) < 12 ||
        trimmed.find(' ') != std::string::npos)
        return;

    for (auto unpackFn : kUnpackStrategies) {
        if ((this->*unpackFn)(trimmed))
            break;
    }
}

// ── Public constructors ───────────────────────────────────────────────────────

DecodedText::DecodedText(GFSK8::Event::Decoded const &ev)
    : DecodedText(ev.data, ev.type, ev.mode,
                  ev.quality < kQualityThreshold,
                  ev.utc, static_cast<int>(ev.frequency),
                  ev.snr, ev.xdt)
{}

DecodedText::DecodedText(std::string const &frame, int const bits,
                         int const submode)
    : DecodedText(frame, bits, submode, false, 0, 0, 0.0f, 0.0f)
{}

// ── Public methods ────────────────────────────────────────────────────────────

std::vector<std::string> DecodedText::messageWords() const
{
    std::vector<std::string> result;
    result.push_back(m_message);              // index 0: whole message
    auto words = tokenize(m_message, ' ');
    result.insert(result.end(), words.begin(), words.end());
    return result;
}
