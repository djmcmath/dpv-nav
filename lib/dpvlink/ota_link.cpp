#include "ota_link.h"

#include <string.h>

namespace otalink {

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void putU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void putU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

size_t encodeFrame(uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t* out, size_t outLen) {
    if (len > MAX_PAYLOAD) return 0;
    size_t total = HEADER_LEN + len + CRC_LEN;
    if (outLen < total) return 0;
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    putU16(out + 2, seq);
    putU16(out + 4, len);
    if (len > 0) memcpy(out + HEADER_LEN, payload, len);
    putU32(out + HEADER_LEN + len, crc32(out + 2, 4 + len));
    return total;
}

size_t encodeAck(bool ok, uint16_t seq, uint8_t* out, size_t outLen) {
    if (outLen < ACK_LEN) return 0;
    out[0] = ok ? ACK_OK : ACK_NAK;
    putU16(out + 1, seq);
    out[3] = (uint8_t)(out[0] ^ out[1] ^ out[2] ^ 0xFF);
    return ACK_LEN;
}

FrameParser::Result FrameParser::feed(uint8_t b) {
    switch (state_) {
        case State::MAGIC0:
            if (b == MAGIC0) state_ = State::MAGIC1;
            return Result::NONE;

        case State::MAGIC1:
            if (b == MAGIC1) {
                state_ = State::HEADER;
                pos_   = 0;
            } else {
                state_ = (b == MAGIC0) ? State::MAGIC1 : State::MAGIC0;
            }
            return Result::NONE;

        case State::HEADER:
            buf_[pos_++] = b;
            if (pos_ == 4) {
                seq_ = (uint16_t)(buf_[0] | (buf_[1] << 8));
                len_ = (uint16_t)(buf_[2] | (buf_[3] << 8));
                if (len_ > MAX_PAYLOAD) {
                    state_ = State::MAGIC0;  // not a frame -- resync
                    return Result::NONE;
                }
                crcPos_ = 0;
                state_  = (len_ == 0) ? State::CRC : State::PAYLOAD;
            }
            return Result::NONE;

        case State::PAYLOAD:
            buf_[pos_++] = b;
            if (pos_ == 4u + len_) {
                crcPos_ = 0;
                state_  = State::CRC;
            }
            return Result::NONE;

        case State::CRC: {
            crcBytes_[crcPos_++] = b;
            if (crcPos_ < CRC_LEN) return Result::NONE;
            state_ = State::MAGIC0;
            uint32_t got = (uint32_t)crcBytes_[0] | ((uint32_t)crcBytes_[1] << 8) |
                           ((uint32_t)crcBytes_[2] << 16) | ((uint32_t)crcBytes_[3] << 24);
            return (got == crc32(buf_, 4u + len_)) ? Result::FRAME : Result::BAD_CRC;
        }
    }
    return Result::NONE;
}

bool AckParser::feed(uint8_t b) {
    if (n_ == 0 && b != ACK_OK && b != ACK_NAK) return false;
    win_[n_++] = b;
    if (n_ < ACK_LEN) return false;

    if ((uint8_t)(win_[0] ^ win_[1] ^ win_[2] ^ 0xFF) == win_[3]) {
        ok_  = (win_[0] == ACK_OK);
        seq_ = (uint16_t)(win_[1] | (win_[2] << 8));
        n_   = 0;
        return true;
    }
    // Bad check: drop the first byte and rescan the rest for a start byte.
    size_t keep = 0;
    for (size_t i = 1; i < ACK_LEN; i++) {
        if (keep == 0 && win_[i] != ACK_OK && win_[i] != ACK_NAK) continue;
        win_[keep++] = win_[i];
    }
    n_ = keep;
    return false;
}

}  // namespace otalink
