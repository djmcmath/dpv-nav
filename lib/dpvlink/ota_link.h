#pragma once

#include <cstddef>
#include <cstdint>

// Display firmware transfer over the nav<->display UART -- protocol v1.
//
// FROZEN. Any nav build must be able to flash any display build, including
// displays that are several releases behind, so nothing below may change
// meaning. A future incompatible protocol gets a new PROTOCOL_VERSION ("pv" in
// the begin packet) and the display refuses versions it doesn't know.
//
// Sequence (nav drives; stop-and-wait):
//   nav  -> {"t":"U","pv":1,"size":N,"sha256":"<hex>","v":"x.y.z"}\n
//   disp -> {"cmd":41,"ok":1}\n                 (OTA_READY; ok:0 + "err" refuses)
//   nav  -> frame seq 0, 1, 2 ...               each answered by a 4-byte ACK/NAK
//   nav  -> frame with len 0                    end of image
//   disp -> {"cmd":42,"ok":1}\n  then restarts  (OTA_DONE; ok:0 + "err" = rejected)
//   nav  -> frame seq SEQ_ABORT, len 0          at any point: give up, back to normal
//
// Frame:  A5 5A | seq u16 LE | len u16 LE (<= MAX_PAYLOAD) | payload | crc32 u32 LE
//         crc32 (IEEE 802.3, as zlib) covers seq, len and payload -- not the magic.
// ACK:    'A' or 'N' | seq u16 LE | check    where check = type ^ seq_lo ^ seq_hi ^ 0xFF
//
// The display ACKs a frame only after its payload is in flash, so nav never
// sends into a busy receiver. A repeat of the frame it just ACKed (the ACK was
// lost) is ACKed again without being written twice.
namespace otalink {

constexpr uint8_t  PROTOCOL_VERSION = 1;
constexpr uint8_t  MAGIC0           = 0xA5;
constexpr uint8_t  MAGIC1           = 0x5A;
constexpr uint16_t MAX_PAYLOAD      = 1024;
constexpr uint16_t SEQ_ABORT        = 0xFFFF;
constexpr size_t   HEADER_LEN       = 6;  // magic(2) + seq(2) + len(2)
constexpr size_t   CRC_LEN          = 4;
constexpr size_t   MAX_FRAME_LEN    = HEADER_LEN + MAX_PAYLOAD + CRC_LEN;
constexpr size_t   ACK_LEN          = 4;
constexpr uint8_t  ACK_OK           = 'A';
constexpr uint8_t  ACK_NAK          = 'N';

// Timing both ends agree on.
constexpr uint32_t ACK_TIMEOUT_MS     = 1000;   // nav: resend a frame after this
constexpr uint8_t  MAX_RETRIES        = 5;      // nav: per frame, and for the begin packet
constexpr uint32_t RECEIVER_IDLE_MS   = 30000;  // display: no valid frame for this long = abort
constexpr uint32_t FRAME_GAP_RESET_MS = 500;    // display: drop a half-received frame after this

// IEEE CRC-32 (zlib's crc32()). Pass the previous result to continue a running CRC.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);

// Writes one frame into out. Returns bytes written, or 0 if it doesn't fit or
// len > MAX_PAYLOAD. payload may be null when len is 0.
size_t encodeFrame(uint16_t seq, const uint8_t* payload, uint16_t len, uint8_t* out, size_t outLen);

size_t encodeAck(bool ok, uint16_t seq, uint8_t* out, size_t outLen);

// Byte-at-a-time frame decoder. Garbage between frames (a JSON line, line noise)
// is skipped until the next magic.
class FrameParser {
public:
    enum class Result : uint8_t { NONE, FRAME, BAD_CRC };

    Result feed(uint8_t b);
    void   reset() { state_ = State::MAGIC0; }
    bool   midFrame() const { return state_ != State::MAGIC0; }

    // Valid after feed() returned FRAME (seq/len also after BAD_CRC).
    uint16_t       seq() const { return seq_; }
    uint16_t       len() const { return len_; }
    const uint8_t* payload() const { return buf_ + 4; }

private:
    enum class State : uint8_t { MAGIC0, MAGIC1, HEADER, PAYLOAD, CRC };
    State    state_ = State::MAGIC0;
    uint8_t  buf_[4 + MAX_PAYLOAD];  // seq + len + payload, the CRC'd bytes
    size_t   pos_ = 0;
    uint16_t seq_ = 0;
    uint16_t len_ = 0;
    uint8_t  crcBytes_[CRC_LEN];
    size_t   crcPos_ = 0;
};

// Byte-at-a-time ACK/NAK decoder; resynchronizes by sliding one byte on a bad check.
class AckParser {
public:
    // True when a complete, valid ACK/NAK has just been decoded.
    bool     feed(uint8_t b);
    void     reset() { n_ = 0; }
    bool     ok() const { return ok_; }
    uint16_t seq() const { return seq_; }

private:
    uint8_t  win_[ACK_LEN];
    size_t   n_   = 0;
    bool     ok_  = false;
    uint16_t seq_ = 0;
};

}  // namespace otalink
