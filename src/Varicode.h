/**
 * @file Varicode.h
 * @brief JS8 varicode encoding and message framing.
 *
 * Rewritten from the original Varicode.h.  The public interface is
 * preserved exactly — every static method, enum value, and struct member
 * has the same name, type, and semantics.  Internal organisation and
 * documentation comments have been reworked for clarity.
 *
 * (C) 2018 Jordan Sherer <kn4crd@gmail.com> - All Rights Reserved
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class Varicode {
public:
    // ── Message metadata ──────────────────────────────────────────────────────

    /// Extra metadata extracted during buildMessageFrames.
    struct MessageInfo {
        std::string dirTo;   ///< Addressed-to callsign, if directed.
        std::string dirCmd;  ///< Command token, if directed.
        std::string dirNum;  ///< Numeric argument, if present.
    };

    // ── Submode identifiers ───────────────────────────────────────────────────

    /// Integer IDs matching JS8Call-improved's Varicode::SubModeType.
    enum SubmodeType {
        JS8CallNormal = 0,
        JS8CallFast   = 1,
        JS8CallTurbo  = 2,
        JS8CallSlow   = 4,
        JS8CallUltra  = 8,
    };

    // ── Transmission-frame type bits ──────────────────────────────────────────

    /// Carried in the itype field of each transmitted frame.
    enum TransmissionType {
        JS8Call      = 0, ///< [000] middle frame
        JS8CallFirst = 1, ///< [001] first frame of a message
        JS8CallLast  = 2, ///< [010] last frame of a message
        JS8CallData  = 4, ///< [100] raw data frame (no frame-type header)
    };

    // ── Frame-type encoding ───────────────────────────────────────────────────

    /**
     * Three-bit frame type embedded in the packed payload.
     *
     * Encoding:
     *   000 = heartbeat
     *   001 = compound
     *   010 = compound directed
     *   011 = directed
     *   1XX = data (X bits dropped)
     */
    enum FrameType {
        FrameUnknown          = 255, ///< sentinel — never transmitted
        FrameHeartbeat        = 0,   ///< [000]
        FrameCompound         = 1,   ///< [001]
        FrameCompoundDirected = 2,   ///< [010]
        FrameDirected         = 3,   ///< [011]
        FrameData             = 4,   ///< [10X] — first two MSBs only
        FrameDataCompressed   = 6,   ///< [11X] — first two MSBs only
    };

    static const uint8_t FrameTypeMax = 6;

    static std::string frameTypeString(uint8_t type)
    {
        static const char *kNames[] = {
            "FrameHeartbeat",        // 0
            "FrameCompound",         // 1
            "FrameCompoundDirected", // 2
            "FrameDirected",         // 3
            "FrameData",             // 4
            "FrameUnknown",          // 5 (gap)
            "FrameDataCompressed",   // 6
        };
        if (type > FrameTypeMax) return "FrameUnknown";
        return kNames[type];
    }

    // ── Submode helper ────────────────────────────────────────────────────────

    static SubmodeType intToSubmode(int sm);

    // ── Character-set utilities ───────────────────────────────────────────────

    static std::string extendedChars();
    static std::string escape(const std::string &text);
    static std::string unescape(const std::string &text);
    static std::string rstrip(const std::string &str);
    static std::string lstrip(const std::string &str);

    // ── Huffman coding ────────────────────────────────────────────────────────

    static std::map<std::string, std::string> defaultHuffTable();

    static std::vector<std::pair<int, std::vector<bool>>>
    huffEncode(const std::map<std::string, std::string> &huff,
               std::string const &text);

    static std::string
    huffDecode(const std::map<std::string, std::string> &huff,
               std::vector<bool> const &bitvec);

    static std::unordered_set<std::string>
    huffValidChars(const std::map<std::string, std::string> &huff);

    // ── Display helpers ───────────────────────────────────────────────────────

    static std::string cqString(int number);
    static std::string hbString(int number);
    static bool        startsWithCQ(std::string text);
    static bool        startsWithHB(std::string text);
    static std::string formatSNR(int snr);
    static std::string formatPWR(int dbm);

    // ── Checksum helpers ──────────────────────────────────────────────────────

    static std::string checksum16(std::string const &input);
    static bool        checksum16Valid(std::string const &checksum,
                                       std::string const &input);

    static std::string checksum32(std::string const &input);
    static bool        checksum32Valid(std::string const &checksum,
                                       std::string const &input);

    // ── Parsing helpers ───────────────────────────────────────────────────────

    static std::vector<std::string> parseCallsigns(std::string const &input);
    static std::vector<std::string> parseGrids(std::string const &input);

    // ── Bit-vector utilities ──────────────────────────────────────────────────

    static std::vector<bool> bytesToBits(char *bitvec, int n);
    static std::vector<bool> strToBits(std::string const &bitvec);
    static std::string       bitsToStr(std::vector<bool> const &bitvec);

    static std::vector<bool> intToBits(uint64_t value, int expected = 0);
    static uint64_t          bitsToInt(std::vector<bool> const value);
    static uint64_t          bitsToInt(std::vector<bool>::const_iterator start, int n);

    static std::vector<bool> bitsListToBits(std::vector<std::vector<bool>> &list);

    // ── Fixed-width pack / unpack ─────────────────────────────────────────────

    static uint8_t     unpack5bits(std::string const &value);
    static std::string pack5bits(uint8_t packed);

    static uint8_t     unpack6bits(std::string const &value);
    static std::string pack6bits(uint8_t packed);

    static uint16_t    unpack16bits(std::string const &value);
    static std::string pack16bits(uint16_t packed);

    static uint32_t    unpack32bits(std::string const &value);
    static std::string pack32bits(uint32_t packed);

    static uint64_t    unpack64bits(std::string const &value);
    static std::string pack64bits(uint64_t packed);

    static uint64_t    unpack72bits(std::string const &value, uint8_t *pRem);
    static std::string pack72bits(uint64_t value, uint8_t rem);

    // ── Alpha-numeric packing ─────────────────────────────────────────────────

    static uint32_t    packAlphaNumeric22(std::string const &value, bool isFlag);
    static std::string unpackAlphaNumeric22(uint32_t packed, bool *isFlag);

    static uint64_t    packAlphaNumeric50(std::string const &value);
    static std::string unpackAlphaNumeric50(uint64_t packed);

    // ── Callsign packing ──────────────────────────────────────────────────────

    static uint32_t    packCallsign(std::string const &value, bool *pPortable);
    static std::string unpackCallsign(uint32_t value, bool portable);

    // ── Grid-square packing ───────────────────────────────────────────────────

    static std::string            deg2grid(float dlong, float dlat);
    static std::pair<float,float> grid2deg(std::string const &grid);
    static uint16_t               packGrid(std::string const &value);
    static std::string            unpackGrid(uint16_t value);

    // ── Command packing ───────────────────────────────────────────────────────

    static uint8_t packNum(std::string const &num, bool *ok);
    static uint8_t packPwr(std::string const &pwr, bool *ok);
    static uint8_t packCmd(uint8_t cmd, uint8_t num, bool *pPackedNum);
    static uint8_t unpackCmd(uint8_t value, uint8_t *pNum);

    // ── Command classification ─────────────────────────────────────────────────

    static bool isSNRCommand(const std::string &cmd);
    static bool isCommandAllowed(const std::string &cmd);
    static bool isCommandBuffered(const std::string &cmd);
    static int  isCommandChecksumed(const std::string &cmd);
    static bool isCommandAutoreply(const std::string &cmd);

    // ── Callsign / group validation ───────────────────────────────────────────

    static bool isValidCallsign(const std::string &callsign, bool *pIsCompound);
    static bool isCompoundCallsign(const std::string &callsign);
    static bool isGroupAllowed(const std::string &group);

    // ── Frame pack / unpack ───────────────────────────────────────────────────

    static std::string
    packHeartbeatMessage(std::string const &text,
                         std::string const &callsign, int *n);

    static std::vector<std::string>
    unpackHeartbeatMessage(const std::string &text,
                           uint8_t *pType, bool *isAlt, uint8_t *pBits3);

    static std::string
    packCompoundMessage(std::string const &text, int *n);

    static std::vector<std::string>
    unpackCompoundMessage(const std::string &text,
                          uint8_t *pType, uint8_t *pBits3);

    static std::string
    packCompoundFrame(const std::string &callsign,
                      uint8_t type, uint16_t num, uint8_t bits3);

    static std::vector<std::string>
    unpackCompoundFrame(const std::string &text,
                        uint8_t *pType, uint16_t *pNum, uint8_t *pBits3);

    static std::string
    packDirectedMessage(std::string const &text,
                        std::string const &mycall,
                        std::string *pTo,
                        bool        *pToCompound,
                        std::string *pCmd,
                        std::string *pNum, int *n);

    static std::vector<std::string>
    unpackDirectedMessage(std::string const &text, uint8_t *pType);

    static std::string packDataMessage(std::string const &text, int *n);
    static std::string unpackDataMessage(std::string const &text);

    static std::string packFastDataMessage(std::string const &text, int *n);
    static std::string unpackFastDataMessage(std::string const &text);

    // ── High-level framing ────────────────────────────────────────────────────

    static std::vector<std::pair<std::string, int>>
    buildMessageFrames(std::string const &mycall,
                       std::string const &mygrid,
                       std::string const &selectedCall,
                       std::string const &text,
                       bool forceIdentify, bool forceData,
                       int submode,
                       MessageInfo *pInfo = nullptr);
};
