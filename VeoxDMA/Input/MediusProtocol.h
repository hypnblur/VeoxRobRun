#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Medius {
constexpr uint8_t kSof = 0xA5;
constexpr uint8_t kProtoVer = 9; // Current Medius 3.4.2+ wire/ABI
constexpr uint8_t kLegacyProtoVer = 8; // Medius 3.4.1
inline bool IsSupportedVersion(uint8_t version)
{
    return version == kProtoVer || version == kLegacyProtoVer;
}
constexpr uint32_t kBaud = 6000000;
constexpr size_t kMaxPayload = 512;
constexpr uint8_t kTypeMove = 0x01;
constexpr uint8_t kTypeInject = 0x03;
constexpr uint8_t kTypeReset = 0x04;
constexpr uint8_t kTypeQuery = 0x05;
constexpr uint8_t kTypeResp = 0x06;
constexpr uint8_t kTypeCatch = 0x0B;
constexpr uint8_t kTypeUsageEvent = 0x0F;
constexpr uint8_t kQueryVersion = 0;
constexpr uint8_t kQueryHealth = 1;
constexpr uint8_t kClassButton = 0;
constexpr uint8_t kClassKey = 1;
constexpr uint8_t kActSoftRel = 0;
constexpr uint8_t kActPress = 1;
constexpr uint8_t kActForceRel = 2;

inline uint16_t Crc16Ccitt(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

inline size_t EncodeFrame(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len,
    uint8_t* out, size_t cap)
{
    if (len > kMaxPayload || cap < 7 + len) return 0;
    uint8_t crcInput[4 + kMaxPayload];
    crcInput[0] = type;
    crcInput[1] = seq;
    crcInput[2] = static_cast<uint8_t>(len);
    crcInput[3] = static_cast<uint8_t>(len >> 8);
    if (len) memcpy(crcInput + 4, payload, len);
    const uint16_t crc = Crc16Ccitt(crcInput, 4 + len);
    out[0] = kSof;
    out[1] = type;
    out[2] = seq;
    out[3] = crcInput[2];
    out[4] = crcInput[3];
    if (len) memcpy(out + 5, payload, len);
    out[5 + len] = static_cast<uint8_t>(crc);
    out[6 + len] = static_cast<uint8_t>(crc >> 8);
    return 7 + len;
}

struct Decoder {
    enum class Step { Sof, Type, Seq, LenLo, LenHi, Payload, CrcLo, CrcHi };
    Step step = Step::Sof;
    uint8_t type = 0, seq = 0;
    uint16_t len = 0, crcRx = 0;
    size_t got = 0;
    uint8_t payload[kMaxPayload]{};

    void Reset() { *this = {}; }

    bool Feed(uint8_t b, uint8_t& outType, uint8_t& outSeq, const uint8_t*& outPayload, size_t& outLen)
    {
        switch (step) {
        case Step::Sof:
            if (b == kSof) step = Step::Type;
            return false;
        case Step::Type:
            type = b; step = Step::Seq; return false;
        case Step::Seq:
            seq = b; step = Step::LenLo; return false;
        case Step::LenLo:
            len = b; step = Step::LenHi; return false;
        case Step::LenHi:
            len = static_cast<uint16_t>(len | (static_cast<uint16_t>(b) << 8));
            got = 0;
            if (len > kMaxPayload) { Reset(); return false; }
            step = len ? Step::Payload : Step::CrcLo;
            return false;
        case Step::Payload:
            payload[got++] = b;
            if (got >= len) step = Step::CrcLo;
            return false;
        case Step::CrcLo:
            crcRx = b; step = Step::CrcHi; return false;
        case Step::CrcHi: {
            crcRx = static_cast<uint16_t>(crcRx | (static_cast<uint16_t>(b) << 8));
            uint8_t crcInput[4 + kMaxPayload];
            crcInput[0] = type;
            crcInput[1] = seq;
            crcInput[2] = static_cast<uint8_t>(len);
            crcInput[3] = static_cast<uint8_t>(len >> 8);
            if (len) memcpy(crcInput + 4, payload, len);
            const bool ok = Crc16Ccitt(crcInput, 4 + len) == crcRx;
            outType = type;
            outSeq = seq;
            outPayload = payload;
            outLen = len;
            step = Step::Sof;
            got = 0;
            type = 0;
            seq = 0;
            crcRx = 0;
            len = 0;
            return ok;
        }
        }
        return false;
    }
};
} // namespace Medius
