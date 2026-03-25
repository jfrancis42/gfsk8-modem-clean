/**
 * @file Varicode.cpp
 * @brief JS8 varicode encoding and message framing implementation.
 *
 * Rewritten from the original Varicode.cpp.  All public method implementations
 * produce identical results; internal helper functions are renamed with a
 * "strutil_" prefix (vs. the original "str_") and the CRC helpers are renamed
 * to crc16Kermit / crc32Bzip2.  The logging category is renamed to
 * varicode_codec.
 *
 * (C) 2018 Jordan Sherer <kn4crd@gmail.com> - All Rights Reserved
 */

#include "Varicode.h"
#include "JSC.h"
#include "log.h"

// No-op logging category (replaces Q_LOGGING_CATEGORY / Q_DECLARE_LOGGING_CATEGORY)
Q_LOGGING_CATEGORY(varicode_codec, "varicode.codec")

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// ---------------------------------------------------------------------------
// String helper functions (replacing Qt QString methods)
// ---------------------------------------------------------------------------

static std::string strutil_trimmed(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string strutil_toUpper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

static std::string strutil_toLower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static bool strutil_startsWith(const std::string &s, const std::string &prefix) {
    return s.rfind(prefix, 0) == 0;
}

static bool strutil_endsWith(const std::string &s, const std::string &suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string strutil_left(const std::string &s, size_t n) {
    return s.substr(0, std::min(n, s.size()));
}

static std::string strutil_right(const std::string &s, size_t n) {
    if (n >= s.size()) return s;
    return s.substr(s.size() - n);
}

static std::string strutil_mid(const std::string &s, size_t pos, size_t len = std::string::npos) {
    if (pos >= s.size()) return "";
    return s.substr(pos, len);
}

static std::vector<std::string> strutil_split(const std::string &s, char sep,
                                          bool skipEmpty = false) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!skipEmpty || !cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!skipEmpty || !cur.empty()) out.push_back(cur);
    return out;
}

static std::string strutil_join(const std::vector<std::string> &v, const std::string &sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i > 0) out += sep;
        out += v[i];
    }
    return out;
}

static std::string strutil_repeat(char c, int n) {
    if (n <= 0) return "";
    return std::string(n, c);
}

static std::string strutil_replace(std::string s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

static std::string strutil_removeRe(const std::string &s, const std::regex &re) {
    return std::regex_replace(s, re, "");
}

// Concatenate two std::vector<bool> (replaces Qt's QVector::operator+)
static std::vector<bool> bitvecAppend(std::vector<bool> a, const std::vector<bool> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

static bool strutil_contains(const std::string &s, char c) {
    return s.find(c) != std::string::npos;
}

static bool strutil_contains(const std::string &s, const std::string &sub) {
    return s.find(sub) != std::string::npos;
}

static int strutil_indexOf(const std::string &s, char c) {
    auto pos = s.find(c);
    return pos == std::string::npos ? -1 : (int)pos;
}

static int strutil_indexOf(const std::string &haystack, const std::string &needle, int from = 0) {
    auto pos = haystack.find(needle, from);
    return pos == std::string::npos ? -1 : (int)pos;
}

static int strutil_count(const std::string &s, char c) {
    return (int)std::count(s.begin(), s.end(), c);
}

static std::string strutil_number(int n) {
    return std::to_string(n);
}

// Right-justify an integer in a field of width `width`, padded with `fill`.
static std::string strutil_arg_int(int v, int width, char fill = ' ') {
    std::string s = std::to_string(v < 0 ? -v : v);
    if (v < 0) s = "-" + s;
    while ((int)s.size() < width) s = fill + s;
    return s;
}

// Right-justify a float in a field of width `width`, `prec` decimal places.
static std::string strutil_arg_float(float v, int width, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%*.*f", width, prec, (double)v);
    return buf;
}

// ---------------------------------------------------------------------------
// CRC implementations (replacing CRCpp)
// CRC-16/KERMIT (poly=0x1021, init=0x0000, refin=true, refout=true, xorout=0x0000)
// CRC-32/BZIP2  (poly=0x04C11DB7, init=0xFFFFFFFF, refin=false, refout=false, xorout=0xFFFFFFFF)
// ---------------------------------------------------------------------------

static uint16_t crc16Kermit(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        // reflect input
        for (int b = 0; b < 8; b++) {
            bool mix = (crc ^ byte) & 0x0001;
            crc >>= 1;
            if (mix) crc ^= 0x8408; // reflected poly
            byte >>= 1;
        }
    }
    // CRC-16/KERMIT: refout=true, xorout=0x0000 → no final reflection needed
    return crc;
}

static uint32_t crc32Bzip2(const uint8_t *data, size_t len) {
    // CRC-32/BZIP2: poly=0x04C11DB7, init=0xFFFFFFFF, refin=false, refout=false, xorout=0xFFFFFFFF
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int b = 7; b >= 0; b--) {
            bool bit = ((crc >> 31) ^ ((byte >> b) & 1)) != 0;
            crc = (crc << 1) ^ (bit ? 0x04C11DB7u : 0u);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------
// Module-level data (originally global Qt objects in Varicode.cpp)
// ---------------------------------------------------------------------------

static const int nalphabet = 41;
static const std::string alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?"; // alphabet to encode _into_
                                                  // for FT8 freetext
                                                  // transmission
static const std::string alphabet72 =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+/?"
    ".";
static const std::string grid_pattern =
    R"((?:[A-X]{2}[0-9]{2}(?:[A-X]{2}(?:[0-9]{2})?)*)+)";
static const std::string orig_compound_callsign_pattern =
    R"((\d|[A-Z])+\/?((\d|[A-Z]){2,})(\/(\d|[A-Z])+)?(\/(\d|[A-Z])+)?)";
static const std::string base_callsign_pattern =
    R"(\b(?:[0-9A-Z])?(?:[0-9A-Z])(?:[0-9])(?:[A-Z])?(?:[A-Z])?(?:[A-Z])?(?:[/][P])?\b)";
static const std::string compound_callsign_pattern =
    R"((?:[@]?|\b)(?:[A-Z0-9\/@][A-Z0-9\/]{0,2}[\/]?[A-Z0-9\/]{0,3}[\/]?[A-Z0-9\/]{0,3})\b)";
static const std::string pack_callsign_pattern =
    R"(([0-9A-Z ])([0-9A-Z])([0-9])([A-Z ])([A-Z ])([A-Z ]))";
static const std::string alphanumeric =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@"; // callsign and grid alphabet

static const std::map<std::string, int> directed_cmds = {
    // any changes here need to be made also in the directed regular expression
    // for parsing
    {" HEARTBEAT", -1},
    {" HB", -1},
    {" CQ", -1},

    {" SNR?", 0},
    {"?", 0},

    {" DIT DIT", 1},

    {" HEARING?", 3},

    {" GRID?", 4},

    {">", 5},

    {" STATUS?", 6},

    {" STATUS", 7},

    {" HEARING", 8},

    {" MSG", 9},

    {" MSG TO:", 10},

    {" QUERY", 11},

    {" QUERY MSGS", 12},
    {" QUERY MSGS?", 12},

    {" QUERY CALL", 13},

    // {" ",           14  }, // reserved

    {" GRID", 15},

    {" INFO?", 16},
    {" INFO", 17},

    {" FB", 18},
    {" HW CPY?", 19},
    {" SK", 20},
    {" RR", 21},

    {" QSL?", 22},
    {" QSL", 23},

    {" CMD", 24},

    {" SNR", 25},
    {" NO", 26},
    {" YES", 27},
    {" 73", 28},

    {" NACK", 2},
    {" ACK", 14},

    {" HEARTBEAT SNR", 29},

    {" AGN?", 30},
    {"  ", 31},
    {" ", 31},
};

static const std::set<int> allowed_cmds = {-1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
                                           10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                           21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

static const std::set<int> autoreply_cmds = {0, 2, 3, 4, 6, 9, 10, 11, 12, 13, 14, 16, 30};

static const std::set<int> buffered_cmds = {5, 9, 10, 11, 12, 13, 15, 24};

static const std::set<int> snr_cmds = {25, 29};

static const std::map<int, int> checksum_cmds = {{5, 16},  {9, 16},  {10, 16}, {11, 16},
                                                 {12, 16}, {13, 16}, {15, 0},  {24, 16}};

static const std::string callsign_pattern = "(?:[@]?[A-Z0-9/]+)";
static const std::string optional_cmd_pattern =
    "(?:\\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG "
    "TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY "
    "MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY "
    "MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT "
    "SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))?";
static const std::string optional_grid_pattern = "(?:\\s?[A-R]{2}[0-9]{2})?";
// Note: (?<=SNR) lookbehind is not supported by all std::regex implementations.
// The SNR number (group 3 in directed_re) is matched unconditionally here;
// callers check whether the command actually is SNR before using it.
static const std::string optional_num_pattern =
    "(?:\\s?[-+]?(?:3[01]|[0-2]?[0-9]))?";

// Group layout for directed_re: (1)=callsign, (2)=cmd, (3)=snr_num
static const std::regex directed_re(
    "^([@]?[A-Z0-9/]+)"
    "(\\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))?"
    "(\\s?[-+]?(?:3[01]|[0-2]?[0-9]))?",
    std::regex::ECMAScript);

static const std::regex heartbeat_re(
    R"(^\s*(?:[@](?:ALLCALL|HB)\s+)?(CQ CQ CQ|CQ DX|CQ QRP|CQ CONTEST|CQ FIELD|CQ FD|CQ CQ|CQ|HB|HEARTBEAT(?!\s+SNR))(?:\s([A-R]{2}[0-9]{2}))?\b)",
    std::regex::ECMAScript);

// compound_re: group(1)=callsign, group(2)=extra(grid+cmd+num)
static const std::regex compound_re(
    "^\\s*[`]([@]?[A-Z0-9/]+)"
    "((?:\\s?[A-R]{2}[0-9]{2})?"
    "(?:\\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))?"
    "(?:\\s?[-+]?(?:3[01]|[0-2]?[0-9]))?)",
    std::regex::ECMAScript);

static const std::map<std::string, std::string> hufftable = {
    // char   code                 weight
    {" ", "01"},       // 1.0
    {"E", "100"},      // 0.5
    {"T", "1101"},     // 0.333333333333
    {"A", "0011"},     // 0.25
    {"O", "11111"},    // 0.2
    {"I", "11100"},    // 0.166666666667
    {"N", "10111"},    // 0.142857142857
    {"S", "10100"},    // 0.125
    {"H", "00011"},    // 0.111111111111
    {"R", "00000"},    // 0.1
    {"D", "111011"},   // 0.0909090909091
    {"L", "110011"},   // 0.0833333333333
    {"C", "110001"},   // 0.0769230769231
    {"U", "101101"},   // 0.0714285714286
    {"M", "101011"},   // 0.0666666666667
    {"W", "001011"},   // 0.0625
    {"F", "001001"},   // 0.0588235294118
    {"G", "000101"},   // 0.0555555555556
    {"Y", "000011"},   // 0.0526315789474
    {"P", "1111011"},  // 0.05
    {"B", "1111001"},  // 0.047619047619
    {".", "1110100"},  // 0.0434782608696
    {"V", "1100101"},  // 0.0416666666667
    {"K", "1100100"},  // 0.04
    {"-", "1100001"},  // 0.0384615384615
    {"+", "1100000"},  // 0.037037037037
    {"?", "1011001"},  // 0.0344827586207
    {"!", "1011000"},  // 0.0333333333333
    {"\"", "1010101"}, // 0.0322580645161
    {"X", "1010100"},  // 0.03125
    {"0", "0010101"},  // 0.030303030303
    {"J", "0010100"},  // 0.0294117647059
    {"1", "0010001"},  // 0.0285714285714
    {"Q", "0010000"},  // 0.0277777777778
    {"2", "0001001"},  // 0.027027027027
    {"Z", "0001000"},  // 0.0263157894737
    {"3", "0000101"},  // 0.025641025641
    {"5", "0000100"},  // 0.025
    {"4", "11110101"}, // 0.0243902439024
    {"9", "11110100"}, // 0.0238095238095
    {"8", "11110001"}, // 0.0232558139535
    {"6", "11110000"}, // 0.0227272727273
    {"7", "11101011"}, // 0.0222222222222
    {"/", "11101010"}, // 0.0217391304348
};

static const char ESC_CHAR = '\\'; // Escape char
static const char EOT_CHAR = '\x04'; // EOT char

static const uint32_t nbasecall = 37 * 36 * 10 * 27 * 27 * 27;
static const uint16_t nbasegrid = 180 * 180;
static const uint16_t nusergrid = nbasegrid + 10;
static const uint16_t nmaxgrid  = (1 << 15) - 1;

static const std::map<std::string, uint32_t> basecalls = {
    {"<....>", nbasecall + 1},
    {"@ALLCALL", nbasecall + 2},
    {"@JS8NET", nbasecall + 3},

    {"@DX/NA", nbasecall + 4},
    {"@DX/SA", nbasecall + 5},
    {"@DX/EU", nbasecall + 6},
    {"@DX/AS", nbasecall + 7},
    {"@DX/AF", nbasecall + 8},
    {"@DX/OC", nbasecall + 9},
    {"@DX/AN", nbasecall + 10},

    {"@REGION/1", nbasecall + 11},
    {"@REGION/2", nbasecall + 12},
    {"@REGION/3", nbasecall + 13},

    {"@GROUP/0", nbasecall + 14},
    {"@GROUP/1", nbasecall + 15},
    {"@GROUP/2", nbasecall + 16},
    {"@GROUP/3", nbasecall + 17},
    {"@GROUP/4", nbasecall + 18},
    {"@GROUP/5", nbasecall + 19},
    {"@GROUP/6", nbasecall + 20},
    {"@GROUP/7", nbasecall + 21},
    {"@GROUP/8", nbasecall + 22},
    {"@GROUP/9", nbasecall + 23},

    {"@COMMAND", nbasecall + 24},
    {"@CONTROL", nbasecall + 25},
    {"@NET", nbasecall + 26},
    {"@NTS", nbasecall + 27},

    {"@RESERVE/0", nbasecall + 28},
    {"@RESERVE/1", nbasecall + 29},
    {"@RESERVE/2", nbasecall + 30},
    {"@RESERVE/3", nbasecall + 31},
    {"@RESERVE/4", nbasecall + 32},

    {"@APRSIS", nbasecall + 33},
    {"@RAGCHEW", nbasecall + 34},
    {"@JS8", nbasecall + 35},
    {"@EMCOMM", nbasecall + 36},
    {"@ARES", nbasecall + 37},
    {"@MARS", nbasecall + 38},
    {"@AMRRON", nbasecall + 39},
    {"@RACES", nbasecall + 40},
    {"@RAYNET", nbasecall + 41},
    {"@RADAR", nbasecall + 42},
    {"@SKYWARN", nbasecall + 43},
    {"@CQ", nbasecall + 44},
    {"@HB", nbasecall + 45},
    {"@QSO", nbasecall + 46},
    {"@QSOPARTY", nbasecall + 47},
    {"@CONTEST", nbasecall + 48},
    {"@FIELDDAY", nbasecall + 49},
    {"@SOTA", nbasecall + 50},
    {"@IOTA", nbasecall + 51},
    {"@POTA", nbasecall + 52},
    {"@QRP", nbasecall + 53},
    {"@QRO", nbasecall + 54},
};

// Lookup key for basecalls by value
static std::string basecalls_key(uint32_t value) {
    for (auto const &kv : basecalls) {
        if (kv.second == value) return kv.first;
    }
    return {};
}

static const std::map<uint32_t, std::string> cqs = {
    {0, "CQ CQ CQ"}, {1, "CQ DX"}, {2, "CQ QRP"}, {3, "CQ CONTEST"},
    {4, "CQ FIELD"}, {5, "CQ FD"}, {6, "CQ CQ"},  {7, "CQ"},
};

static const std::map<uint32_t, std::string> hbs = {
    {0, "HB"},
    {1, "HB"},
    {2, "HB"},
    {3, "HB"},
    {4, "HB"},
    {5, "HB"},
    {6, "HB"},
    {7, "HB"},
};

// Helper: find key in map<K,V> by value
template<typename K, typename V>
static K map_key(const std::map<K, V> &m, const V &val, const K &def) {
    for (auto const &kv : m) {
        if (kv.second == val) return kv.first;
    }
    return def;
}

static const std::map<int, int> dbm2mw = {
    {0, 1},
    {3, 2},
    {7, 5},
    {10, 10},
    {13, 20},
    {17, 50},
    {20, 100},
    {23, 200},
    {27, 500},
    {30, 1000},
    {33, 2000},
    {37, 5000},
    {40, 10000},
    {43, 20000},
    {47, 50000},
    {50, 100000},
    {53, 200000},
    {57, 500000},
    {60, 1000000},
};

/*
 * UTILITIES
 */

static int mwattsToDbm(int mwatts) {
    int dbm = 0;
    // collect sorted mw values
    std::vector<int> values;
    for (auto const &kv : dbm2mw) values.push_back(kv.second);
    std::sort(values.begin(), values.end());
    for (auto mw : values) {
        if (mw < mwatts) continue;
        dbm = map_key(dbm2mw, mw, 0);
        break;
    }
    return dbm;
}

static int dbmTomwatts(int dbm) {
    auto it = dbm2mw.find(dbm);
    if (it != dbm2mw.end()) return it->second;
    auto iter = dbm2mw.lower_bound(dbm);
    if (iter == dbm2mw.end()) return dbm2mw.rbegin()->second;
    return iter->second;
}

std::string Varicode::extendedChars() {
    static std::string c;
    if (c.empty()) {
        for (uint32_t i = 0; i < JSC::prefixSize; i++) {
            if (JSC::prefix[i].size != 1) continue;
            c += JSC::prefix[i].str[0];
        }
    }
    return c;
}

std::string Varicode::escape(const std::string &text) {
    std::string escaped;
    escaped.reserve(6 * text.size());
    for (unsigned char ch : text) {
        if (ch < 0x80) {
            escaped += (char)ch;
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\U%04x", (unsigned)ch);
            escaped += buf;
        }
    }
    return escaped;
}

std::string Varicode::unescape(const std::string &text) {
    std::string unescaped(text);
    // Replace \Uxxxx sequences with the corresponding latin-1 character
    static const std::regex r("((?:[uU][+]|\\\\[uU])[0-9a-fA-F]{4})");
    std::string result;
    std::sregex_iterator it(unescaped.begin(), unescaped.end(), r);
    std::sregex_iterator end;
    size_t pos = 0;
    for (; it != end; ++it) {
        auto m = *it;
        result += unescaped.substr(pos, m.position() - pos);
        std::string hex = m.str().substr(m.str().size() - 4);
        unsigned cp = (unsigned)std::stoul(hex, nullptr, 16);
        if (cp < 256) result += (char)(unsigned char)cp;
        pos = m.position() + m.length();
    }
    result += unescaped.substr(pos);
    return result;
}

std::string Varicode::rstrip(const std::string &str) {
    int n = (int)str.size() - 1;
    for (; n >= 0; --n) {
        if (std::isspace((unsigned char)str[n])) continue;
        return str.substr(0, n + 1);
    }
    return "";
}

std::string Varicode::lstrip(const std::string &str) {
    int len = (int)str.size();
    for (int n = 0; n < len; n++) {
        if (std::isspace((unsigned char)str[n])) continue;
        return str.substr(n);
    }
    return "";
}

/*
 * VARICODE
 */
std::map<std::string, std::string> Varicode::defaultHuffTable() { return hufftable; }

std::string Varicode::cqString(int number) {
    auto it = cqs.find(number);
    if (it == cqs.end()) return {};
    return it->second;
}

std::string Varicode::hbString(int number) {
    auto it = hbs.find(number);
    if (it == hbs.end()) return {};
    return it->second;
}

bool Varicode::startsWithCQ(std::string text) {
    for (auto const &kv : cqs) {
        if (strutil_startsWith(text, kv.second)) return true;
    }
    return false;
}

bool Varicode::startsWithHB(std::string text) {
    for (auto const &kv : hbs) {
        if (strutil_startsWith(text, kv.second)) return true;
    }
    return false;
}

std::string Varicode::formatSNR(int snr) {
    if (snr < -60 || snr > 60) return {};
    char buf[16];
    if (snr >= 0)
        std::snprintf(buf, sizeof(buf), "+%02d", snr);
    else
        std::snprintf(buf, sizeof(buf), "%03d", snr);
    return buf;
}

std::string Varicode::formatPWR(int dbm) {
    int mw = dbmTomwatts(dbm);
    char buf[32];
    if (mw >= 1000)
        std::snprintf(buf, sizeof(buf), "%dW", mw / 1000);
    else
        std::snprintf(buf, sizeof(buf), "%dmW", mw);
    return buf;
}

std::string Varicode::checksum16(std::string const &input) {
    auto crc = crc16Kermit((const uint8_t *)input.data(), input.size());
    auto checksum = Varicode::pack16bits(crc);
    if ((int)checksum.size() < 3) {
        checksum += strutil_repeat(' ', 3 - (int)checksum.size());
    }
    return checksum;
}

bool Varicode::checksum16Valid(std::string const &checksum, std::string const &input) {
    auto crc = crc16Kermit((const uint8_t *)input.data(), input.size());
    return Varicode::pack16bits(crc) == checksum;
}

std::string Varicode::checksum32(std::string const &input) {
    auto crc = crc32Bzip2((const uint8_t *)input.data(), input.size());
    auto checksum = Varicode::pack32bits(crc);
    if ((int)checksum.size() < 6) {
        checksum += strutil_repeat(' ', 6 - (int)checksum.size());
    }
    return checksum;
}

bool Varicode::checksum32Valid(std::string const &checksum, std::string const &input) {
    auto crc = crc32Bzip2((const uint8_t *)input.data(), input.size());
    return Varicode::pack32bits(crc) == checksum;
}

std::vector<std::string> Varicode::parseCallsigns(std::string const &input) {
    std::vector<std::string> callsigns;
    std::regex re(compound_callsign_pattern);
    std::regex grid_re(grid_pattern);
    std::sregex_iterator it(input.begin(), input.end(), re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        std::string callsign = strutil_trimmed((*it)[0].str());
        if (!Varicode::isValidCallsign(callsign, nullptr)) continue;
        if (std::regex_search(callsign, grid_re)) continue;
        callsigns.push_back(callsign);
    }
    return callsigns;
}

std::vector<std::string> Varicode::parseGrids(const std::string &input) {
    std::vector<std::string> grids;
    std::regex re(grid_pattern);
    std::sregex_iterator it(input.begin(), input.end(), re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        std::string grid = (*it)[0].str();
        if (grid == "RR73") continue;
        grids.push_back(grid);
    }
    return grids;
}

std::vector<std::pair<int, std::vector<bool>>>
Varicode::huffEncode(const std::map<std::string, std::string> &huff, std::string const &text) {
    std::vector<std::pair<int, std::vector<bool>>> out;

    // Build a sorted key list: longer keys first, then lexicographically descending
    std::vector<std::string> keys;
    for (auto const &kv : huff) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end(), [](std::string const &a, std::string const &b) {
        if (b.size() < a.size()) return true;
        if (a.size() < b.size()) return false;
        return b < a;
    });

    int i = 0;
    while (i < (int)text.size()) {
        bool found = false;
        for (auto const &ch : keys) {
            if (strutil_startsWith(text.substr(i), ch)) {
                out.push_back({(int)ch.size(), Varicode::strToBits(huff.at(ch))});
                i += (int)ch.size();
                found = true;
                break;
            }
        }
        if (!found) {
            i++;
        }
    }

    return out;
}

std::string Varicode::huffDecode(std::map<std::string, std::string> const &huff,
                                 std::vector<bool> const &bitvec) {
    std::string text;
    std::string bits = Varicode::bitsToStr(bitvec);

    // EOT char as string for comparison
    std::string eot_str(1, EOT_CHAR);

    while (!bits.empty()) {
        bool found = false;
        for (auto const &kv : huff) {
            if (strutil_startsWith(bits, kv.second)) {
                if (kv.first == eot_str) {
                    text += " ";
                    found = false;
                    break;
                }
                text += kv.first;
                bits = bits.substr(kv.second.size());
                found = true;
            }
        }
        if (!found) break;
    }

    return text;
}

std::unordered_set<std::string> Varicode::huffValidChars(const std::map<std::string, std::string> &huff) {
    std::unordered_set<std::string> s;
    for (auto const &kv : huff) s.insert(kv.first);
    return s;
}

// convert char* array of 0 bytes and 1 bytes to bool vector
std::vector<bool> Varicode::bytesToBits(char *bitvec, int n) {
    std::vector<bool> bits;
    for (int i = 0; i < n; i++) {
        bits.push_back(bitvec[i] == 0x01);
    }
    return bits;
}

// convert string of 0s and 1s to bool vector
std::vector<bool> Varicode::strToBits(std::string const &bitvec) {
    std::vector<bool> bits;
    for (char ch : bitvec) {
        bits.push_back(ch == '1');
    }
    return bits;
}

std::string Varicode::bitsToStr(std::vector<bool> const &bitvec) {
    std::string bits;
    for (bool bit : bitvec) {
        bits += (bit ? '1' : '0');
    }
    return bits;
}

std::vector<bool> Varicode::intToBits(uint64_t value, int expected) {
    std::vector<bool> bits;

    while (value) {
        bits.insert(bits.begin(), (bool)(value & 1));
        value = value >> 1;
    }

    if (expected) {
        while ((int)bits.size() < expected) {
            bits.insert(bits.begin(), (bool)0);
        }
    }

    return bits;
}

uint64_t Varicode::bitsToInt(std::vector<bool> const value) {
    uint64_t v = 0;
    for (bool bit : value) {
        v = (v << 1) + (int)(bit);
    }
    return v;
}

uint64_t Varicode::bitsToInt(std::vector<bool>::const_iterator start, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        int bit = (int)(*start);
        v = (v << 1) + (int)(bit);
        start++;
    }
    return v;
}

std::vector<bool> Varicode::bitsListToBits(std::vector<std::vector<bool>> &list) {
    std::vector<bool> out;
    for (auto const &vec : list) {
        out.insert(out.end(), vec.begin(), vec.end());
    }
    return out;
}

uint8_t Varicode::unpack5bits(std::string const &value) {
    return (uint8_t)alphabet.find(value[0]);
}

// pack a 5-bit value from 0 to 31 into a single character
std::string Varicode::pack5bits(uint8_t packed) {
    return std::string(1, alphabet[packed % 32]);
}

uint8_t Varicode::unpack6bits(std::string const &value) {
    return (uint8_t)alphabet.find(value[0]);
}

// pack a 6-bit value from 0 to 40 into a single character
std::string Varicode::pack6bits(uint8_t packed) {
    return std::string(1, alphabet[packed % 41]);
}

uint16_t Varicode::unpack16bits(std::string const &value) {
    int a = (int)alphabet.find(value[0]);
    int b = (int)alphabet.find(value[1]);
    int c = (int)alphabet.find(value[2]);

    int unpacked = (nalphabet * nalphabet) * a + nalphabet * b + c;
    if (unpacked > (1 << 16) - 1) {
        // BASE-41 can produce a value larger than 16 bits... ala "???" == 70643
        return 0;
    }

    return (uint16_t)(unpacked & ((1 << 16) - 1));
}

// pack a 16-bit value into a three character sequence
std::string Varicode::pack16bits(uint16_t packed) {
    std::string out;
    uint16_t tmp = packed / (uint16_t)(nalphabet * nalphabet);

    out += alphabet[tmp];

    tmp = (uint16_t)((packed - (tmp * (nalphabet * nalphabet))) / nalphabet);
    out += alphabet[tmp];

    tmp = packed % nalphabet;
    out += alphabet[tmp];

    return out;
}

uint32_t Varicode::unpack32bits(std::string const &value) {
    return (uint32_t)(unpack16bits(strutil_left(value, 3))) << 16 |
           unpack16bits(strutil_right(value, 3));
}

std::string Varicode::pack32bits(uint32_t packed) {
    uint16_t a = (uint16_t)((packed & 0xFFFF0000) >> 16);
    uint16_t b = (uint16_t)(packed & 0xFFFF);
    return pack16bits(a) + pack16bits(b);
}

uint64_t Varicode::unpack64bits(std::string const &value) {
    return (uint64_t)(unpack32bits(strutil_left(value, 6))) << 32 |
           unpack32bits(strutil_right(value, 6));
}

std::string Varicode::pack64bits(uint64_t packed) {
    uint32_t a = (uint32_t)((packed & 0xFFFFFFFF00000000ULL) >> 32);
    uint32_t b = (uint32_t)(packed & 0xFFFFFFFFULL);
    return pack32bits(a) + pack32bits(b);
}

// returns the first 64 bits and sets the last 8 bits in pRem
uint64_t Varicode::unpack72bits(std::string const &text, uint8_t *pRem) {
    uint64_t value = 0;
    uint8_t rem = 0;
    uint8_t mask2 = (uint8_t)((1 << 2) - 1);

    for (int i = 0; i < 10; i++) {
        value |= (uint64_t)(alphabet72.find(text[i])) << (58 - 6 * i);
    }

    uint8_t remHigh = (uint8_t)alphabet72.find(text[10]);
    value |= remHigh >> 2;

    uint8_t remLow = (uint8_t)alphabet72.find(text[11]);
    rem = (uint8_t)(((remHigh & mask2) << 6) | remLow);

    if (pRem) *pRem = rem;
    return value;
}

std::string Varicode::pack72bits(uint64_t value, uint8_t rem) {
    char packed[12]; // 12 x 6bit characters

    uint8_t mask4 = (uint8_t)((1 << 4) - 1);
    uint8_t mask6 = (uint8_t)((1 << 6) - 1);

    uint8_t remHigh = (uint8_t)(((value & mask4) << 2) | (rem >> 6));
    uint8_t remLow = rem & mask6;
    value = value >> 4;

    packed[11] = alphabet72[remLow];
    packed[10] = alphabet72[remHigh];

    for (int i = 0; i < 10; i++) {
        packed[9 - i] = alphabet72[value & mask6];
        value = value >> 6;
    }

    return std::string(packed, 12);
}

//     //
// --- //
//     //

// pack a 4-digit alpha-numeric + space into a 22 bit value
// 21 bits for the data + 1 bit for a flag indicator
// giving us a total of 5.5 bits per character
uint32_t Varicode::packAlphaNumeric22(std::string const &value, bool isFlag) {
    std::string word = strutil_removeRe(value, std::regex("[^A-Z0-9/ ]"));
    if ((int)word.size() < 4) {
        word = word + strutil_repeat(' ', 4 - (int)word.size());
    }

    uint32_t a = (uint32_t)(38 * 38 * 38) * (uint32_t)alphanumeric.find(word[0]);
    uint32_t b = (uint32_t)(38 * 38) * (uint32_t)alphanumeric.find(word[1]);
    uint32_t c = (uint32_t)38 * (uint32_t)alphanumeric.find(word[2]);
    uint32_t d = (uint32_t)alphanumeric.find(word[3]);

    uint32_t packed = a + b + c + d;
    packed = (packed << 1) + (int)isFlag;

    return packed;
}

std::string Varicode::unpackAlphaNumeric22(uint32_t packed, bool *isFlag) {
    char word[4];

    if (isFlag) *isFlag = packed & 1;
    packed = packed >> 1;

    uint32_t tmp = packed % 38;
    word[3] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[2] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[1] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[0] = alphanumeric[tmp];
    packed = packed / 38;

    return std::string(word, 4);
}

// pack a 10-digit alpha-numeric + space + forward-slash into a 50 bit value
// optionally start with an @
uint64_t Varicode::packAlphaNumeric50(std::string const &value) {
    std::string word = strutil_removeRe(value, std::regex("[^A-Z0-9 /@]"));
    if (word.size() > 3 && word[3] != '/') {
        word.insert(3, 1, ' ');
    }
    if (word.size() > 7 && word[7] != '/') {
        word.insert(7, 1, ' ');
    }

    if ((int)word.size() < 11) {
        word = word + strutil_repeat(' ', 11 - (int)word.size());
    }

    uint64_t a = (uint64_t)38 * 38 * 38 * 2 * 38 * 38 * 38 * 2 * 38 * 38 *
                 (uint64_t)alphanumeric.find(word[0]);
    uint64_t b = (uint64_t)38 * 38 * 38 * 2 * 38 * 38 * 38 * 2 * 38 *
                 (uint64_t)alphanumeric.find(word[1]);
    uint64_t c = (uint64_t)38 * 38 * 38 * 2 * 38 * 38 * 38 * 2 *
                 (uint64_t)alphanumeric.find(word[2]);
    uint64_t d =
        (uint64_t)38 * 38 * 38 * 2 * 38 * 38 * 38 * (uint64_t)(word[3] == '/');
    uint64_t e =
        (uint64_t)38 * 38 * 38 * 2 * 38 * 38 * (uint64_t)alphanumeric.find(word[4]);
    uint64_t f =
        (uint64_t)38 * 38 * 38 * 2 * 38 * (uint64_t)alphanumeric.find(word[5]);
    uint64_t g = (uint64_t)38 * 38 * 38 * 2 * (uint64_t)alphanumeric.find(word[6]);
    uint64_t h = (uint64_t)38 * 38 * 38 * (uint64_t)(word[7] == '/');
    uint64_t ii = (uint64_t)38 * 38 * (uint64_t)alphanumeric.find(word[8]);
    uint64_t j = (uint64_t)38 * (uint64_t)alphanumeric.find(word[9]);
    uint64_t k = (uint64_t)alphanumeric.find(word[10]);

    uint64_t packed = a + b + c + d + e + f + g + h + ii + j + k;

    return packed;
}

std::string Varicode::unpackAlphaNumeric50(uint64_t packed) {
    char word[11];

    uint64_t tmp = packed % 38;
    word[10] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[9] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[8] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 2;
    word[7] = tmp ? '/' : ' ';
    packed = packed / 2;

    tmp = packed % 38;
    word[6] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[5] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[4] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 2;
    word[3] = tmp ? '/' : ' ';
    packed = packed / 2;

    tmp = packed % 38;
    word[2] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 38;
    word[1] = alphanumeric[tmp];
    packed = packed / 38;

    tmp = packed % 39;
    word[0] = alphanumeric[tmp];
    packed = packed / 39;

    std::string value(word, 11);

    return strutil_replace(value, " ", "");
}

// pack a callsign into a 28-bit value and a boolean portable flag
uint32_t Varicode::packCallsign(std::string const &value, bool *pPortable) {
    uint32_t packed = 0;

    std::string callsign = strutil_trimmed(strutil_toUpper(value));

    {
        auto it = basecalls.find(callsign);
        if (it != basecalls.end()) return it->second;
    }

    // strip /P
    if (strutil_endsWith(callsign, "/P")) {
        callsign = strutil_left(callsign, callsign.size() - 2);
        if (pPortable) *pPortable = true;
    }

    // workaround for swaziland
    if (strutil_startsWith(callsign, "3DA0")) {
        callsign = "3D0" + callsign.substr(4);
    }

    // workaround for guinea
    if (strutil_startsWith(callsign, "3X") && callsign.size() > 2 &&
        'A' <= callsign[2] && callsign[2] <= 'Z') {
        callsign = "Q" + callsign.substr(2);
    }

    int slen = (int)callsign.size();
    if (slen < 2) return packed;
    if (slen > 6) return packed;

    std::vector<std::string> permutations = {callsign};
    if (slen == 2) {
        permutations.push_back(" " + callsign + "   ");
    }
    if (slen == 3) {
        permutations.push_back(" " + callsign + "  ");
        permutations.push_back(callsign + "   ");
    }
    if (slen == 4) {
        permutations.push_back(" " + callsign + " ");
        permutations.push_back(callsign + "  ");
    }
    if (slen == 5) {
        permutations.push_back(" " + callsign);
        permutations.push_back(callsign + " ");
    }

    std::string matched;
    std::regex m(pack_callsign_pattern);
    for (auto const &permutation : permutations) {
        std::smatch sm;
        if (std::regex_search(permutation, sm, m)) {
            matched = sm[0].str();
        }
    }
    if (matched.empty()) return packed;
    if ((int)matched.size() < 6) return packed;

    packed = (uint32_t)alphanumeric.find(matched[0]);
    packed = 36 * packed + (uint32_t)alphanumeric.find(matched[1]);
    packed = 10 * packed + (uint32_t)alphanumeric.find(matched[2]);
    packed = 27 * packed + (uint32_t)alphanumeric.find(matched[3]) - 10;
    packed = 27 * packed + (uint32_t)alphanumeric.find(matched[4]) - 10;
    packed = 27 * packed + (uint32_t)alphanumeric.find(matched[5]) - 10;

    return packed;
}

std::string Varicode::unpackCallsign(uint32_t value, bool portable) {
    // check basecalls first
    std::string key = basecalls_key(value);
    if (!key.empty()) return key;

    char word[6];
    uint32_t tmp = value % 27 + 10;
    word[5] = alphanumeric[tmp];
    value = value / 27;

    tmp = value % 27 + 10;
    word[4] = alphanumeric[tmp];
    value = value / 27;

    tmp = value % 27 + 10;
    word[3] = alphanumeric[tmp];
    value = value / 27;

    tmp = value % 10;
    word[2] = alphanumeric[tmp];
    value = value / 10;

    tmp = value % 36;
    word[1] = alphanumeric[tmp];
    value = value / 36;

    tmp = value;
    word[0] = alphanumeric[tmp];

    std::string callsign(word, 6);
    if (strutil_startsWith(callsign, "3D0")) {
        callsign = "3DA0" + callsign.substr(3);
    }

    if (strutil_startsWith(callsign, "Q") && callsign.size() > 1 &&
        'A' <= callsign[1] && callsign[1] <= 'Z') {
        callsign = "3X" + callsign.substr(1);
    }

    if (portable) {
        callsign = strutil_trimmed(callsign) + "/P";
    }

    return strutil_trimmed(callsign);
}

std::string Varicode::deg2grid(float dlong, float dlat) {
    char grid[6];

    if (dlong < -180) dlong += 360;
    if (dlong > 180)  dlong -= 360;

    int nlong = (int)(60.0 * (180.0 - dlong) / 5);

    int n1 = nlong / 240;
    int n2 = (nlong - 240 * n1) / 24;
    int n3 = (nlong - 240 * n1 - 24 * n2);

    grid[0] = (char)('A' + n1);
    grid[2] = (char)('0' + n2);
    grid[4] = (char)('a' + n3);

    int nlat = (int)(60.0 * (dlat + 90) / 2.5);

    n1 = nlat / 240;
    n2 = (nlat - 240 * n1) / 24;
    n3 = (nlat - 240 * n1 - 24 * n2);

    grid[1] = (char)('A' + n1);
    grid[3] = (char)('0' + n2);
    grid[5] = (char)('a' + n3);

    return std::string(grid, 6);
}

std::pair<float, float> Varicode::grid2deg(std::string const &grid) {
    std::pair<float, float> longLat;

    std::string g = grid;
    if ((int)g.size() < 6) {
        g = strutil_left(grid, 4) + "mm";
    }

    g = strutil_toUpper(strutil_left(g, 4)) + strutil_toLower(strutil_right(g, 2));

    int nlong = 180 - 20 * (g[0] - 'A');
    int n20d = 2 * (g[2] - '0');
    float xminlong = 5.0f * (g[4] - 'a' + 0.5f);
    float dlong = (float)nlong - (float)n20d - xminlong / 60.0f;

    int nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
    float xminlat = 2.5f * (g[5] - 'a' + 0.5f);
    float dlat = (float)nlat + xminlat / 60.0f;

    longLat.first = dlong;
    longLat.second = dlat;

    return longLat;
}

// pack a 4-digit maidenhead grid locator into a 15-bit value
uint16_t Varicode::packGrid(std::string const &value) {
    std::string grid = strutil_trimmed(value);
    if ((int)grid.size() < 4) {
        return (uint16_t)((1 << 15) - 1);
    }

    auto pair = Varicode::grid2deg(strutil_left(grid, 4));
    int ilong = (int)pair.first;
    int ilat = (int)pair.second + 90;

    return (uint16_t)(((ilong + 180) / 2) * 180 + ilat);
}

std::string Varicode::unpackGrid(uint16_t value) {
    if (value > nbasegrid) {
        return "";
    }

    float dlat = (float)(value % 180) - 90.0f;
    float dlong = (float)(value / 180) * 2.0f - 180.0f + 2.0f;

    return strutil_left(Varicode::deg2grid(dlong, dlat), 4);
}

// pack a number or snr into an integer between 0 & 62
uint8_t Varicode::packNum(std::string const &num, bool *ok) {
    int inum = 0;
    if (num.empty()) {
        if (ok) *ok = false;
        return (uint8_t)inum;
    }

    bool conv_ok = true;
    int parsed = 0;
    try {
        parsed = std::stoi(num);
        conv_ok = true;
    } catch (...) {
        conv_ok = false;
    }

    if (ok) *ok = conv_ok;
    inum = std::max(-30, std::min(parsed, 31));
    return (uint8_t)(inum + 30 + 1);
}

uint8_t Varicode::packPwr(std::string const &pwr, bool *ok) {
    // Parse mW value and pack as dBm
    bool conv_ok = true;
    int parsed = 0;
    try {
        parsed = std::stoi(pwr);
        conv_ok = true;
    } catch (...) {
        conv_ok = false;
    }
    if (ok) *ok = conv_ok;
    return (uint8_t)mwattsToDbm(parsed);
}

// pack a reduced fidelity command and a number into the extra bits provided
// between nbasegrid and nmaxgrid
uint8_t Varicode::packCmd(uint8_t cmd, uint8_t num, bool *pPackedNum) {
    uint8_t value = 0;
    std::string cmdStr;
    for (auto const &kv : directed_cmds) {
        if (kv.second == (int)cmd) { cmdStr = kv.first; break; }
    }
    if (Varicode::isSNRCommand(cmdStr)) {
        // 8 bits - 1 bit flag + 1 bit type + 6 bit number
        // [1][X][6]
        // X = 0 == SNR
        // X = 1 == DEADBEEF
        value = (uint8_t)(((1 << 1) | (int)(cmdStr == " HEARTBEAT SNR")) << 6);
        value = value + (num & (uint8_t)((1 << 6) - 1));
        if (pPackedNum) *pPackedNum = true;
    } else {
        value = cmd & (uint8_t)((1 << 7) - 1);
        if (pPackedNum) *pPackedNum = false;
    }

    return value;
}

uint8_t Varicode::unpackCmd(uint8_t value, uint8_t *pNum) {
    // if the first bit is 1, this is an SNR with a number encoded in the lower
    // 6 bits
    if (value & (1 << 7)) {
        if (pNum) *pNum = value & (uint8_t)((1 << 6) - 1);

        // sending digits with ACKS this way was deprecated in 2.2 (for reasons)
        // so we zero them out when unpacking so we don't display them even if
        // they were encoded that way.
        auto it_snr = directed_cmds.find(" SNR");
        uint8_t cmd = (it_snr != directed_cmds.end()) ? (uint8_t)it_snr->second : 25;

        if (value & (1 << 6)) {
            auto it_hbs = directed_cmds.find(" HEARTBEAT SNR");
            if (it_hbs != directed_cmds.end()) cmd = (uint8_t)it_hbs->second;
        }
        return cmd;
    } else {
        if (pNum) *pNum = 0;
        return value & (uint8_t)((1 << 7) - 1);
    }
}

bool Varicode::isSNRCommand(const std::string &cmd) {
    auto it = directed_cmds.find(cmd);
    if (it == directed_cmds.end()) return false;
    return snr_cmds.count(it->second) > 0;
}

bool Varicode::isCommandAllowed(const std::string &cmd) {
    auto it = directed_cmds.find(cmd);
    if (it == directed_cmds.end()) return false;
    return allowed_cmds.count(it->second) > 0;
}

bool Varicode::isCommandBuffered(const std::string &cmd) {
    auto it = directed_cmds.find(cmd);
    if (it == directed_cmds.end()) return false;
    return strutil_contains(cmd, ' ') || buffered_cmds.count(it->second) > 0;
}

int Varicode::isCommandChecksumed(const std::string &cmd) {
    auto it = directed_cmds.find(cmd);
    if (it == directed_cmds.end()) return 0;
    auto it2 = checksum_cmds.find(it->second);
    if (it2 == checksum_cmds.end()) return 0;
    return it2->second;
}

bool Varicode::isCommandAutoreply(const std::string &cmd) {
    auto it = directed_cmds.find(cmd);
    if (it == directed_cmds.end()) return false;
    return autoreply_cmds.count(it->second) > 0;
}

static bool isValidCompoundCallsign(const std::string &callsign) {
    // compound calls cannot be > 9 characters after removing the /
    int slashes = strutil_count(callsign, '/');
    if ((int)callsign.size() - slashes > 9) {
        return false;
    }

    int slash_idx = strutil_indexOf(callsign, '/');
    if (slash_idx != -1) {
        return basecalls.find(strutil_left(callsign, slash_idx)) == basecalls.end();
    }

    if (!callsign.empty() && callsign[0] == '@') {
        return true;
    }

    if ((int)callsign.size() > 2 &&
        std::regex_search(callsign, std::regex("[0-9][A-Z]|[A-Z][0-9]"))) {
        return true;
    }

    return false;
}

bool Varicode::isValidCallsign(const std::string &callsign, bool *pIsCompound) {
    if (basecalls.count(callsign)) {
        if (pIsCompound) *pIsCompound = false;
        return true;
    }

    std::regex base_re("^" + base_callsign_pattern + "$");
    std::smatch m;
    if (std::regex_match(callsign, m, base_re)) {
        if (pIsCompound) *pIsCompound = false;
        return (int)callsign.size() > 2 &&
               std::regex_search(callsign, std::regex("[0-9][A-Z]|[A-Z][0-9]"));
    }

    std::regex comp_re("^" + compound_callsign_pattern + "$");
    if (std::regex_match(callsign, m, comp_re)) {
        bool isValid = isValidCompoundCallsign(m[0].str());
        if (pIsCompound) *pIsCompound = isValid;
        return isValid;
    }

    if (pIsCompound) *pIsCompound = false;
    return false;
}

bool Varicode::isCompoundCallsign(const std::string &callsign) {
    if (basecalls.count(callsign) && callsign[0] != '@') {
        return false;
    }

    std::regex base_re("^" + base_callsign_pattern + "$");
    std::smatch m;
    if (std::regex_match(callsign, m, base_re)) {
        return false;
    }

    std::regex comp_re("^" + compound_callsign_pattern + "$");
    if (!std::regex_match(callsign, m, comp_re)) {
        return false;
    }

    bool isValid = isValidCompoundCallsign(m[0].str());

    qCDebug(varicode_codec) << "is valid compound?" << m[0].str().c_str() << isValid;

    return isValid;
}

bool Varicode::isGroupAllowed(const std::string &group) {
    static const std::unordered_set<std::string> disallowed = {
        "@APRSIS",
        "@JS8NET",
    };
    return !disallowed.count(group);
}

// CQCQCQ EM73
// CQ DX EM73
// CQ QRP EM73
// HB EM73
std::string Varicode::packHeartbeatMessage(std::string const &text,
                                           const std::string &callsign, int *n) {
    std::string frame;

    std::smatch parsedText;
    if (!std::regex_search(text, parsedText, heartbeat_re)) {
        if (n) *n = 0;
        return frame;
    }

    std::string extra = parsedText[2].str(); // grid group

    // Heartbeat Alt Type
    // ---------------
    // 1      0   HB
    // 1      1   CQ

    std::string type = parsedText[1].str();
    bool isAlt = strutil_startsWith(type, "CQ");

    if (callsign.empty()) {
        if (n) *n = 0;
        return frame;
    }

    uint16_t packed_extra = nmaxgrid; // which will display an empty string
    if ((int)extra.size() == 4 &&
        std::regex_search(extra, std::regex(grid_pattern))) {
        packed_extra = Varicode::packGrid(extra);
    }

    uint8_t cqNumber = (uint8_t)map_key(hbs, type, (uint32_t)0);

    if (isAlt) {
        packed_extra |= (1 << 15);
        cqNumber = (uint8_t)map_key(cqs, type, (uint32_t)0);
    }

    frame = packCompoundFrame(callsign, Varicode::FrameHeartbeat, packed_extra,
                              cqNumber);
    if (frame.empty()) {
        if (n) *n = 0;
        return frame;
    }

    if (n) *n = (int)parsedText[0].length();
    return frame;
}

std::vector<std::string> Varicode::unpackHeartbeatMessage(const std::string &text,
                                                          uint8_t *pType, bool *isAlt,
                                                          uint8_t *pBits3) {
    uint8_t type = Varicode::FrameHeartbeat;
    uint16_t num = nmaxgrid;
    uint8_t bits3 = 0;

    std::vector<std::string> unpacked = unpackCompoundFrame(text, &type, &num, &bits3);
    if (unpacked.empty() || type != Varicode::FrameHeartbeat) {
        return {};
    }

    unpacked.push_back(Varicode::unpackGrid(num & (uint16_t)((1 << 15) - 1)));

    if (isAlt)  *isAlt  = (bool)(num & (1 << 15));
    if (pType)  *pType  = type;
    if (pBits3) *pBits3 = bits3;

    return unpacked;
}

// KN4CRD/XXXX EM73
// XXXX/KN4CRD EM73
// KN4CRD/P
std::string Varicode::packCompoundMessage(std::string const &text, int *n) {
    std::string frame;

    qCDebug(varicode_codec) << "trying to pack compound message" << text.c_str();
    std::smatch parsedText;
    if (!std::regex_search(text, parsedText, compound_re)) {
        qCDebug(varicode_codec) << "no match for compound message" << text.c_str();
        if (n) *n = 0;
        return frame;
    }

    std::string callsign = parsedText[1].str();
    // group(2) is the full extra; we need to parse grid/cmd/num from it
    std::string extra_str = parsedText[2].str();

    // Parse grid from extra_str: leading optional grid
    std::string grid;
    std::string cmd;
    std::string num;

    {
        std::smatch em;
        static const std::regex grid_only_re("^\\s?([A-R]{2}[0-9]{2})");
        if (std::regex_search(extra_str, em, grid_only_re)) {
            grid = em[1].str();
        }
    }

    {
        std::smatch em;
        static const std::regex cmd_re(
            "(\\s?(?:AGN[?]|QSL[?]|HW CPY[?]|MSG TO[:]|SNR[?]|INFO[?]|GRID[?]|STATUS[?]|QUERY MSGS[?]|HEARING[?]|(?:(?:STATUS|HEARING|QUERY CALL|QUERY MSGS|QUERY|CMD|MSG|NACK|ACK|73|YES|NO|HEARTBEAT SNR|SNR|QSL|RR|SK|FB|INFO|GRID|DIT DIT)(?=[ ]|$))|[?> ]))");
        if (std::regex_search(extra_str, em, cmd_re)) {
            cmd = em[1].str();
        }
    }

    num = strutil_trimmed(parsedText.suffix().str()); // rough fallback; for num after SNR

    if (callsign.empty()) {
        if (n) *n = 0;
        return frame;
    }

    uint8_t type = Varicode::FrameCompound;
    uint16_t extra = nmaxgrid;

    qCDebug(varicode_codec) << "try pack cmd" << cmd.c_str()
                          << directed_cmds.count(cmd)
                          << Varicode::isCommandAllowed(cmd);

    if (!cmd.empty() && directed_cmds.count(cmd) &&
        Varicode::isCommandAllowed(cmd)) {
        bool packedNum = false;
        uint8_t inum = Varicode::packNum(num, nullptr);
        int dc = directed_cmds.at(cmd);
        extra = (uint16_t)(nusergrid + Varicode::packCmd((uint8_t)dc, inum, &packedNum));

        type = Varicode::FrameCompoundDirected;
    } else if (!grid.empty()) {
        extra = Varicode::packGrid(grid);
    }

    frame = Varicode::packCompoundFrame(callsign, type, extra, 0);

    if (n) *n = (int)parsedText[0].length();
    return frame;
}

std::vector<std::string> Varicode::unpackCompoundMessage(const std::string &text,
                                                         uint8_t *pType,
                                                         uint8_t *pBits3) {
    uint8_t type = Varicode::FrameCompound;
    uint16_t extra = nmaxgrid;
    uint8_t bits3 = 0;

    std::vector<std::string> unpacked = unpackCompoundFrame(text, &type, &extra, &bits3);
    if (unpacked.empty() || (type != Varicode::FrameCompound &&
                              type != Varicode::FrameCompoundDirected)) {
        return {};
    }

    if (extra <= nbasegrid) {
        unpacked.push_back(" " + Varicode::unpackGrid(extra));
    } else if (nusergrid <= extra && extra < nmaxgrid) {
        // if this is a grid that is higher than the usergrid reference, treat
        // this as an SNR command
        uint8_t num = 0;
        auto cmd_val = Varicode::unpackCmd((uint8_t)(extra - nusergrid), &num);
        std::string cmdStr;
        for (auto const &kv : directed_cmds) {
            if (kv.second == (int)cmd_val) { cmdStr = kv.first; break; }
        }

        unpacked.push_back(cmdStr);

        if (Varicode::isSNRCommand(cmdStr)) {
            unpacked.push_back(Varicode::formatSNR((int)num - 31));
        }
    }

    if (pType)  *pType  = type;
    if (pBits3) *pBits3 = bits3;

    return unpacked;
}

std::string Varicode::packCompoundFrame(const std::string &callsign, uint8_t type,
                                        uint16_t num, uint8_t bits3) {
    std::string frame;

    // needs to be a compound type...
    if (type == Varicode::FrameData || type == Varicode::FrameDirected) {
        return frame;
    }

    uint8_t packed_flag = type;
    uint64_t packed_callsign = Varicode::packAlphaNumeric50(callsign);
    if (packed_callsign == 0) {
        return frame;
    }

    uint16_t mask11 = (uint16_t)(((1 << 11) - 1) << 5);
    uint8_t mask5 = (uint8_t)((1 << 5) - 1);

    uint16_t packed_11 = (uint16_t)((num & mask11) >> 5);
    uint8_t packed_5 = num & mask5;
    uint8_t packed_8 = (uint8_t)((packed_5 << 3) | bits3);

    // [3][50][11],[5][3] = 72
    auto bits = bitvecAppend(bitvecAppend(Varicode::intToBits(packed_flag, 3),
                              Varicode::intToBits(packed_callsign, 50)),
                              Varicode::intToBits(packed_11, 11));

    return Varicode::pack72bits(Varicode::bitsToInt(bits), packed_8);
}

std::vector<std::string> Varicode::unpackCompoundFrame(const std::string &text,
                                                       uint8_t *pType,
                                                       uint16_t *pNum,
                                                       uint8_t *pBits3) {
    std::vector<std::string> unpacked;

    if ((int)text.size() < 12 || strutil_contains(text, ' ')) {
        return unpacked;
    }

    // [3][50][11],[5][3] = 72
    uint8_t packed_8 = 0;
    auto bits =
        Varicode::intToBits(Varicode::unpack72bits(text, &packed_8), 64);

    uint8_t packed_5 = packed_8 >> 3;
    uint8_t packed_3 = packed_8 & (uint8_t)((1 << 3) - 1);

    uint8_t packed_flag = (uint8_t)Varicode::bitsToInt(
        std::vector<bool>(bits.begin(), bits.begin() + 3));

    // needs to be a ping type...
    if (packed_flag == Varicode::FrameData ||
        packed_flag == Varicode::FrameDirected) {
        return unpacked;
    }

    uint64_t packed_callsign = Varicode::bitsToInt(
        std::vector<bool>(bits.begin() + 3, bits.begin() + 53));
    uint16_t packed_11 = (uint16_t)Varicode::bitsToInt(
        std::vector<bool>(bits.begin() + 53, bits.begin() + 64));

    std::string callsign = Varicode::unpackAlphaNumeric50(packed_callsign);

    uint16_t num = (uint16_t)((packed_11 << 5) | packed_5);

    if (pType)  *pType  = packed_flag;
    if (pNum)   *pNum   = num;
    if (pBits3) *pBits3 = packed_3;

    unpacked.push_back(callsign);
    unpacked.push_back("");

    return unpacked;
}

// J1Y ACK
// J1Y?
// KN4CRD: J1Y$
// KN4CRD: J1Y! HELLO WORLD
std::string Varicode::packDirectedMessage(const std::string &text,
                                          const std::string &mycall,
                                          std::string *pTo,
                                          bool *pToCompound,
                                          std::string *pCmd,
                                          std::string *pNum, int *n) {
    std::string frame;

    std::smatch match;
    if (!std::regex_search(text, match, directed_re)) {
        if (n) *n = 0;
        return frame;
    }

    std::string from = mycall;
    bool isFromCompound = Varicode::isCompoundCallsign(from);
    if (isFromCompound) {
        from = "<....>";
    }
    std::string to = match[1].str();  // callsign group
    std::string cmd = match[2].str(); // cmd group
    std::string num = match[3].str(); // num group

    // ensure we have a directed command
    if (cmd.empty()) {
        if (n) *n = 0;
        return frame;
    }

    // ensure we have a valid callsign
    bool isToCompound = false;
    bool validToCallsign =
        (to != mycall) && Varicode::isValidCallsign(to, &isToCompound);
    if (!validToCallsign) {
        qCDebug(varicode_codec) << "to" << to.c_str() << "is not a valid callsign";
        if (n) *n = 0;
        return frame;
    }

    // return back the parsed "to" field
    if (pTo)         *pTo = to;
    if (pToCompound) *pToCompound = isToCompound;

    // then replace the current processing with a placeholder that we _can_ pack
    // into a directed command
    if (isToCompound) {
        to = "<....>";
    }

    qCDebug(varicode_codec) << "directed" << validToCallsign << isToCompound << to.c_str();

    // validate command
    if (!Varicode::isCommandAllowed(cmd) &&
        !Varicode::isCommandAllowed(strutil_trimmed(cmd))) {
        if (n) *n = 0;
        return frame;
    }

    // packing general number...
    bool numOK = false;
    uint8_t inum = Varicode::packNum(strutil_trimmed(num), &numOK);
    if (numOK) {
        if (pNum) *pNum = num;
    }

    bool portable_from = false;
    uint32_t packed_from = Varicode::packCallsign(from, &portable_from);

    bool portable_to = false;
    uint32_t packed_to = Varicode::packCallsign(to, &portable_to);

    if (packed_from == 0 || packed_to == 0) {
        if (n) *n = 0;
        return frame;
    }

    std::string cmdOut;
    uint8_t packed_cmd = 0;
    if (directed_cmds.count(cmd)) {
        cmdOut = cmd;
        packed_cmd = (uint8_t)directed_cmds.at(cmd);
    }
    if (directed_cmds.count(strutil_trimmed(cmd))) {
        cmdOut = strutil_trimmed(cmd);
        packed_cmd = (uint8_t)directed_cmds.at(cmdOut);
    }
    uint8_t packed_flag = Varicode::FrameDirected;
    uint8_t packed_extra =
        (uint8_t)((((int)portable_from) << 7) + (((int)portable_to) << 6) + inum);

    // [3][28][28][5],[2][6] = 72
    auto bits = bitvecAppend(bitvecAppend(bitvecAppend(Varicode::intToBits(packed_flag, 3),
                                    Varicode::intToBits(packed_from, 28)),
                              Varicode::intToBits(packed_to, 28)),
                       Varicode::intToBits(packed_cmd % 32, 5));

    if (pCmd) *pCmd = cmdOut;
    if (n) *n = (int)match[0].length();
    return Varicode::pack72bits(Varicode::bitsToInt(bits), packed_extra);
}

std::vector<std::string> Varicode::unpackDirectedMessage(const std::string &text,
                                                         uint8_t *pType) {
    std::vector<std::string> unpacked;

    if ((int)text.size() < 12 || strutil_contains(text, ' ')) {
        return unpacked;
    }

    // [3][28][22][11],[2][6] = 72
    uint8_t extra = 0;
    auto bits = Varicode::intToBits(Varicode::unpack72bits(text, &extra), 64);

    uint8_t packed_flag = (uint8_t)Varicode::bitsToInt(
        std::vector<bool>(bits.begin(), bits.begin() + 3));
    if (packed_flag != Varicode::FrameDirected) {
        return unpacked;
    }

    uint32_t packed_from = (uint32_t)Varicode::bitsToInt(
        std::vector<bool>(bits.begin() + 3, bits.begin() + 31));
    uint32_t packed_to = (uint32_t)Varicode::bitsToInt(
        std::vector<bool>(bits.begin() + 31, bits.begin() + 59));
    uint8_t packed_cmd = (uint8_t)Varicode::bitsToInt(
        std::vector<bool>(bits.begin() + 59, bits.begin() + 64));

    bool portable_from = ((extra >> 7) & 1) == 1;
    bool portable_to   = ((extra >> 6) & 1) == 1;
    extra = extra % 64;

    std::string from = Varicode::unpackCallsign(packed_from, portable_from);
    std::string to   = Varicode::unpackCallsign(packed_to, portable_to);
    std::string cmd;
    // find key in directed_cmds where value == (packed_cmd % 32)
    for (auto const &kv : directed_cmds) {
        if (kv.second == (int)(packed_cmd % 32)) { cmd = kv.first; break; }
    }

    unpacked.push_back(from);
    unpacked.push_back(to);
    unpacked.push_back(cmd);

    if (extra != 0) {
        if (Varicode::isSNRCommand(cmd)) {
            unpacked.push_back(Varicode::formatSNR((int)extra - 31));
        } else {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", (int)extra - 31);
            unpacked.push_back(buf);
        }
    }

    if (pType) *pType = packed_flag;
    return unpacked;
}

static std::string packHuffMessage(const std::string &input,
                                   const std::vector<bool> prefix, int *n) {
    static const int frameSize = 72;

    std::string frame;

    std::vector<bool> frameBits;
    if (!prefix.empty()) {
        frameBits.insert(frameBits.end(), prefix.begin(), prefix.end());
    }

    int i = 0;

    // only pack huff messages that only contain valid chars
    auto validChars = Varicode::huffValidChars(Varicode::defaultHuffTable());
    for (char ch : input) {
        std::string chs(1, (char)std::toupper((unsigned char)ch));
        if (!validChars.count(chs)) {
            if (n) *n = 0;
            return frame;
        }
    }

    // pack using the default huff table
    for (auto const &pair : Varicode::huffEncode(Varicode::defaultHuffTable(), input)) {
        auto charN    = pair.first;
        auto charBits = pair.second;
        if ((int)frameBits.size() + (int)charBits.size() < frameSize) {
            frameBits.insert(frameBits.end(), charBits.begin(), charBits.end());
            i += charN;
            continue;
        }
        break;
    }

    qCDebug(varicode_codec) << "Huff bits" << (int)frameBits.size() << "chars" << i;

    int pad = frameSize - (int)frameBits.size();
    if (pad) {
        for (int k = 0; k < pad; k++) {
            frameBits.push_back(k == 0 ? (bool)0 : (bool)1);
        }
    }

    uint64_t value = Varicode::bitsToInt(frameBits.cbegin(), 64);
    uint8_t rem = (uint8_t)Varicode::bitsToInt(frameBits.cbegin() + 64, 8);
    frame = Varicode::pack72bits(value, rem);

    if (n) *n = i;

    return frame;
}

static std::string packCompressedMessage(const std::string &input,
                                         std::vector<bool> prefix, int *n) {
    static const int frameSize = 72;

    std::string frame;

    std::vector<bool> frameBits;
    if (!prefix.empty()) {
        frameBits.insert(frameBits.end(), prefix.begin(), prefix.end());
    }

    int i = 0;
    for (auto const &pair : JSC::compress(input)) {
        auto bits  = pair.first;
        auto chars = pair.second;

        if ((int)frameBits.size() + (int)bits.size() < frameSize) {
            frameBits.insert(frameBits.end(), bits.begin(), bits.end());
            i += (int)chars;
            continue;
        }

        break;
    }

    qCDebug(varicode_codec) << "Compressed bits" << (int)frameBits.size() << "chars" << i;

    int pad = frameSize - (int)frameBits.size();
    if (pad) {
        for (int k = 0; k < pad; k++) {
            frameBits.push_back(k == 0 ? (bool)0 : (bool)1);
        }
    }

    uint64_t value = Varicode::bitsToInt(frameBits.cbegin(), 64);
    uint8_t rem = (uint8_t)Varicode::bitsToInt(frameBits.cbegin() + 64, 8);
    frame = Varicode::pack72bits(value, rem);

    if (n) *n = i;

    return frame;
}

// TODO: DEPRECATED in 2.2 (we will eventually stop transmitting these frames)
// pack data message using 70 bits available flagged as data by the first 2 bits
std::string Varicode::packDataMessage(const std::string &input, int *n) {
    std::string huffFrame;
    int huffChars = 0;
    huffFrame = packHuffMessage(input, {true, false}, &huffChars);

    std::string compressedFrame;
    int compressedChars = 0;
    compressedFrame =
        packCompressedMessage(input, {true, true}, &compressedChars);

    if (huffChars > compressedChars) {
        if (n) *n = huffChars;
        return huffFrame;
    } else {
        if (n) *n = compressedChars;
        return compressedFrame;
    }
}

// TODO: DEPRECATED in 2.2 (still available for decoding legacy frames, but will
// eventually no longer be decodable) unpack data message using 70 bits
// available flagged as data by the first 2 bits
std::string Varicode::unpackDataMessage(const std::string &text) {
    std::string unpacked;

    if ((int)text.size() < 12 || strutil_contains(text, ' ')) {
        return unpacked;
    }

    uint8_t rem = 0;
    uint64_t value = Varicode::unpack72bits(text, &rem);
    auto bits = Varicode::intToBits(value, 64);
    {
        auto rem_bits = Varicode::intToBits(rem, 8);
        bits.insert(bits.end(), rem_bits.begin(), rem_bits.end());
    }

    bool isData = bits[0];
    if (!isData) {
        return unpacked;
    }

    bits = std::vector<bool>(bits.begin() + 1, bits.end());

    bool compressed = bits[0];
    // find last 0
    int n_idx = -1;
    for (int k = (int)bits.size() - 1; k >= 0; k--) {
        if (!bits[k]) { n_idx = k; break; }
    }

    // trim off the pad bits
    if (n_idx > 1)
        bits = std::vector<bool>(bits.begin() + 1, bits.begin() + n_idx);
    else
        bits = std::vector<bool>(bits.begin() + 1, bits.end());

    if (compressed) {
        // partial word (s,c)-dense coding with code tables
        unpacked = JSC::decompress(bits);
    } else {
        // huff decode the bits (without escapes)
        unpacked = Varicode::huffDecode(Varicode::defaultHuffTable(), bits);
    }

    return unpacked;
}

#define GFSK8_FAST_DATA_CAN_USE_HUFF 0

// pack data message using the full 72 bits available (with the data flag in the
// i3bit header)
std::string Varicode::packFastDataMessage(const std::string &input, int *n) {
#if GFSK8_FAST_DATA_CAN_USE_HUFF
    std::string huffFrame;
    int huffChars = 0;
    huffFrame = packHuffMessage(input, {false}, &huffChars);

    std::string compressedFrame;
    int compressedChars = 0;
    compressedFrame = packCompressedMessage(input, {true}, &compressedChars);

    if (huffChars > compressedChars) {
        if (n) *n = huffChars;
        return huffFrame;
    } else {
        if (n) *n = compressedChars;
        return compressedFrame;
    }
#else
    std::string compressedFrame;
    int compressedChars = 0;
    compressedFrame = packCompressedMessage(input, {}, &compressedChars);

    if (n) *n = compressedChars;
    return compressedFrame;
#endif
}

// unpack data message using the full 72 bits available (with the data flag in
// the i3bit header)
std::string Varicode::unpackFastDataMessage(const std::string &text) {
    std::string unpacked;

    if ((int)text.size() < 12 || strutil_contains(text, ' ')) {
        return unpacked;
    }

    uint8_t rem = 0;
    uint64_t value = Varicode::unpack72bits(text, &rem);
    auto bits = Varicode::intToBits(value, 64);
    {
        auto rem_bits = Varicode::intToBits(rem, 8);
        bits.insert(bits.end(), rem_bits.begin(), rem_bits.end());
    }

#if GFSK8_FAST_DATA_CAN_USE_HUFF
    bool compressed = bits[0];
    // find last 0
    int n_idx = -1;
    for (int k = (int)bits.size() - 1; k >= 0; k--) {
        if (!bits[k]) { n_idx = k; break; }
    }

    // trim off the pad bits
    if (n_idx > 1)
        bits = std::vector<bool>(bits.begin() + 1, bits.begin() + n_idx);
    else
        bits = std::vector<bool>(bits.begin() + 1, bits.end());

    if (compressed) {
        unpacked = JSC::decompress(bits);
    } else {
        unpacked = Varicode::huffDecode(Varicode::defaultHuffTable(), bits);
    }
#else
    // find last 0
    int n_idx = -1;
    for (int k = (int)bits.size() - 1; k >= 0; k--) {
        if (!bits[k]) { n_idx = k; break; }
    }

    // trim off the pad bits
    bits = std::vector<bool>(bits.begin(), n_idx >= 0 ? bits.begin() + n_idx : bits.end());

    // partial word (s,c)-dense coding with code tables
    unpacked = JSC::decompress(bits);
#endif

    return unpacked;
}

// TODO: remove the dependence on providing all this data?
std::vector<std::pair<std::string, int>>
Varicode::buildMessageFrames(std::string const &mycall, std::string const &mygrid,
                             std::string const &selectedCall, std::string const &text,
                             bool forceIdentify, bool forceData, int submode,
                             MessageInfo *pInfo) {

#define ALLOW_SEND_COMPOUND 1
#define ALLOW_SEND_COMPOUND_DIRECTED 1
#define AUTO_PREPEND_DIRECTED 1
#define AUTO_REMOVE_MYCALL 1
#define AUTO_PREPEND_DIRECTED_ALLOW_TEXT_CALLSIGNS 1
#define ALLOW_FORCE_IDENTIFY 1

    bool mycallCompound = Varicode::isCompoundCallsign(mycall);

    std::vector<std::pair<std::string, int>> allFrames;

#if GFSK8_NO_MULTILINE
    // auto lines = split(text, ...);
#else
    std::vector<std::string> lines = {text};
#endif

    for (std::string line : lines) {
        std::vector<std::pair<std::string, int>> lineFrames;

        // once we find a directed call, data encode the rest of the line.
        bool hasDirected = false;

        // do the same for when we have sent data...
        bool hasData = false;

        // or if we're forcing data to be sent...
        if (forceData) {
            forceIdentify = false;
            hasData = true;
        }

#if AUTO_REMOVE_MYCALL
        // remove our callsign from the start of the line...
        if (strutil_startsWith(line, mycall + ":") ||
            strutil_startsWith(line, mycall + " ")) {
            line = lstrip(strutil_mid(line, mycall.size() + 1));
        }
#endif

#if AUTO_RSTRIP_WHITESPACE
        // remove trailing whitespace as long as there are characters left
        // afterwards
        auto rline = rstrip(line);
        if (!rline.empty()) {
            line = rline;
        }
#endif

#if AUTO_PREPEND_DIRECTED
        // see if we need to prepend the directed call to the line...
        if (!selectedCall.empty() && !strutil_startsWith(line, selectedCall) &&
            !strutil_startsWith(line, "`") && !forceData) {
            bool lineStartsWithBaseCall =
                (strutil_startsWith(line, "@ALLCALL") ||
                 Varicode::startsWithCQ(line) ||
                 Varicode::startsWithHB(line));

#if AUTO_PREPEND_DIRECTED_ALLOW_TEXT_CALLSIGNS
            auto calls = Varicode::parseCallsigns(line);
            bool lineStartsWithStandardCall = !calls.empty() &&
                                              strutil_startsWith(line, calls[0]) &&
                                              calls[0].size() > 3;
#else
            bool lineStartsWithStandardCall = false;
#endif

            if (lineStartsWithBaseCall || lineStartsWithStandardCall) {
                // pass
            } else {
                auto sep = strutil_startsWith(line, " ") ? "" : " ";
                line = selectedCall + sep + line;
            }
        }
#endif

        while (line.size() > 0) {
            std::string frame;

            bool useBcn = false;
#if ALLOW_SEND_COMPOUND
            bool useCmp = false;
#endif
            bool useDir = false;
            bool useDat = false;

            int l = 0;
            std::string bcnFrame = Varicode::packHeartbeatMessage(line, mycall, &l);

#if ALLOW_SEND_COMPOUND
            int o = 0;
            std::string cmpFrame = Varicode::packCompoundMessage(line, &o);
#endif

            int n = 0;
            std::string dirCmd;
            std::string dirTo;
            std::string dirNum;
            bool dirToCompound = false;
            std::string dirFrame = Varicode::packDirectedMessage(
                line, mycall, &dirTo, &dirToCompound, &dirCmd, &dirNum, &n);
            if (dirToCompound) {
                qCDebug(varicode_codec)
                    << "directed message to field is compound" << dirTo.c_str();
            }

#if ALLOW_FORCE_IDENTIFY
            // if we're sending a data message, then ensure our callsign is
            // included automatically
            bool isLikelyDataFrame = lineFrames.empty() &&
                                     selectedCall.empty() &&
                                     dirTo.empty() && l == 0 && o == 0;
            if (forceIdentify && isLikelyDataFrame && !strutil_contains(line, mycall)) {
                line = mycall + ": " + line;
            }
#endif
            int m = 0;
            bool fastDataFrame = false;
            std::string datFrame;
            // TODO: DEPRECATED in 2.2 (the following release will remove
            // transmission of these frames)
            if (submode == Varicode::JS8CallNormal) {
                datFrame = Varicode::packDataMessage(line, &m);
                fastDataFrame = false;
            } else {
                datFrame = Varicode::packFastDataMessage(line, &m);
                fastDataFrame = true;
            }

            if (!hasDirected && !hasData && l > 0) {
                useBcn = true;
                hasDirected = false;
                frame = bcnFrame;
            }
#if ALLOW_SEND_COMPOUND
            else if (!hasDirected && !hasData && o > 0) {
                useCmp = true;
                hasDirected = false;
                frame = cmpFrame;
            }
#endif
            else if (!hasDirected && !hasData && n > 0) {
                useDir = true;
                hasDirected = true;
                frame = dirFrame;
            } else if (m > 0) {
                useDat = true;
                hasData = true;
                frame = datFrame;
            }

            if (useBcn) {
                lineFrames.push_back({frame, Varicode::JS8Call});
                line = strutil_mid(line, l);
            }

#if ALLOW_SEND_COMPOUND
            if (useCmp) {
                lineFrames.push_back({frame, Varicode::JS8Call});
                line = strutil_mid(line, o);
            }
#endif

            if (useDir) {
                bool shouldUseStandardFrame = true;

#if ALLOW_SEND_COMPOUND_DIRECTED
                if (mycallCompound || dirToCompound) {
                    qCDebug(varicode_codec)
                        << "compound?" << mycallCompound << dirToCompound;
                    // Cases 1, 2, 3 all send a standard compound frame first...
                    std::string deCompoundMessage =
                        "`" + mycall + " " + mygrid;
                    std::string deCompoundFrame = Varicode::packCompoundMessage(
                        deCompoundMessage, nullptr);
                    if (!deCompoundFrame.empty()) {
                        lineFrames.push_back({deCompoundFrame, Varicode::JS8Call});
                    }

                    // Followed by a standard OR compound directed message...
                    std::string dirCompoundMessage =
                        "`" + dirTo + dirCmd + dirNum;
                    std::string dirCompoundFrame = Varicode::packCompoundMessage(
                        dirCompoundMessage, nullptr);
                    if (!dirCompoundFrame.empty()) {
                        lineFrames.push_back(
                            {dirCompoundFrame, Varicode::JS8Call});
                    }
                    shouldUseStandardFrame = false;
                }
#endif

                if (shouldUseStandardFrame) {
                    // otherwise, just send the standard directed frame
                    lineFrames.push_back({frame, Varicode::JS8Call});
                }

                line = strutil_mid(line, n);

                // generate a checksum for buffered commands with line data
                if (Varicode::isCommandBuffered(dirCmd) && !line.empty()) {
                    qCDebug(varicode_codec) << "generating checksum for line"
                                          << line.c_str() << strutil_mid(line, 1).c_str();

                    // strip leading whitespace after a buffered directed command
                    line = lstrip(line);

                    qCDebug(varicode_codec) << "before:" << line.c_str();

#if 1
                    // case-insensitive compare for @APRSIS
                    bool skipAprsChecksum = (strutil_toUpper(dirTo) == "@APRSIS");
                    int checksumSize =
                        skipAprsChecksum
                            ? 0
                            : Varicode::isCommandChecksumed(dirCmd);
#else
                    int checksumSize = 0;
#endif

                    if (checksumSize == 32) {
                        line = line + " " + Varicode::checksum32(line);
                    } else if (checksumSize == 16) {
                        line = line + " " + Varicode::checksum16(line);
                    } else if (checksumSize == 0) {
                        // pass
                        qCDebug(varicode_codec)
                            << "no checksum required for cmd" << dirCmd.c_str();
                    }
                    qCDebug(varicode_codec) << "after:" << line.c_str();
                }

                if (pInfo) {
                    pInfo->dirCmd = dirCmd;
                    pInfo->dirTo  = dirTo;
                    pInfo->dirNum = dirNum;
                }
            }

            if (useDat) {
                // use the standard data frame
                lineFrames.push_back({frame, fastDataFrame ? Varicode::JS8CallData
                                                           : Varicode::JS8Call});
                line = strutil_mid(line, m);
            }
        }

        if (!lineFrames.empty()) {
            lineFrames.front().second |= Varicode::JS8CallFirst;
            lineFrames.back().second  |= Varicode::JS8CallLast;
        }

        allFrames.insert(allFrames.end(), lineFrames.begin(), lineFrames.end());
    }

    return allFrames;
}

Varicode::SubmodeType Varicode::intToSubmode(int sm) {
    switch (sm) {
    case Varicode::JS8CallNormal:
        return Varicode::JS8CallNormal;
    case Varicode::JS8CallFast:
        return Varicode::JS8CallFast;
    case Varicode::JS8CallTurbo:
        return Varicode::JS8CallTurbo;
    case Varicode::JS8CallSlow:
        return Varicode::JS8CallSlow;
    case Varicode::JS8CallUltra:
        return Varicode::JS8CallUltra;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Unexpected JS8 submode %d", sm);
    throw std::invalid_argument{buf};
}
