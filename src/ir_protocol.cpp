/*
 * IR protocol engine -- see ir_protocol.h for the model.
 *
 * Reference for every timing and bit layout below: the Flipper Zero firmware
 * (lib/infrared/encoder_decoder), which is what the 20 000 remote files in
 * Flipper-IRDB were encoded against, cross-checked against IRremoteESP8266 for
 * the airborne timings.  Flipper compatibility is the point: a `.ir` file taken
 * from that database has to go out of this radio unchanged.
 *
 * Two encoder shapes cover every protocol here:
 *
 *   PDWM (pulse distance / width modulation)  NEC, NECext, Samsung32, SIRC,
 *   Kaseikyo.  A fixed preamble, then per bit a mark and a space where either
 *   the mark length (Sony) or the space length (NEC family, Panasonic) carries
 *   the value.
 *
 *   Manchester  RC5, RC5X, RC6.  Each bit is two half-bit cells of equal
 *   length; the transition in the middle of the cell carries the value.
 *
 * Polarity is not cosmetic.  Flipper's RC5 encoder inverts its payload before
 * Manchester encoding, so an RC5 "1" leaves this firmware as space-then-mark,
 * while an RC6 "1" (no inversion) leaves as mark-then-space.  Both are
 * reproduced here exactly as that firmware emits them, and the decoders know
 * which polarity each protocol uses.
 */

#include "ir_protocol.h"

#include <cstdio>
#include <cstring>

namespace ir {
namespace {

// ---------------------------------------------------------------------------
// Run-length accumulator
//
// The protocol bodies below emit marks and spaces in whatever order their bit
// layout produces, including two spaces in a row (Manchester bit boundaries).
// This merges those runs, keeps the list alternating, and finally drops a
// leading space: a frame that begins with a space has no carrier to suppress,
// so the receiver's view and the transmitter's view of it are identical from
// the first mark on.
// ---------------------------------------------------------------------------
struct Enc {
    Timings* out;
    int n          = 0;
    bool firstMark = true;
    bool curMark   = true;
    bool overflow  = false;

    explicit Enc(Timings* t) : out(t) {}

    void add(bool mark, uint32_t us)
    {
        if (us == 0) return;
        if (us > 0xFFFFu) us = 0xFFFFu;

        if (n == 0) {
            firstMark    = mark;
            curMark      = mark;
            out->us[n++] = static_cast<uint16_t>(us);
            return;
        }
        if (curMark == mark) {  // same level: one longer run
            const uint32_t sum = static_cast<uint32_t>(out->us[n - 1]) + us;
            out->us[n - 1]      = static_cast<uint16_t>(sum > 0xFFFFu ? 0xFFFFu : sum);
            return;
        }
        if (n >= kMaxTimings) {
            overflow = true;
            return;
        }
        curMark      = mark;
        out->us[n++] = static_cast<uint16_t>(us);
    }

    // Store the result into out->count.  Returns false when the frame does not
    // fit or is empty.
    bool finish(uint32_t frequency, uint8_t duty)
    {
        if (overflow || n == 0) return false;

        const int first = firstMark ? 0 : 1;  // drop a leading space
        const int count = n - first;
        if (count < 2) return false;

        if (first != 0) {
            for (int i = 0; i < count; ++i) out->us[i] = out->us[i + first];
        }
        out->count       = count;
        out->frequency   = frequency;
        out->dutyPercent = duty;
        return true;
    }
};

// Shared PDWM timing table.
struct PdwmSpec {
    uint16_t preambleMark;
    uint16_t preambleSpace;
    uint16_t bit1Mark;
    uint16_t bit1Space;
    uint16_t bit0Mark;
    uint16_t bit0Space;
    bool     markEncoded;  // true: the mark length carries the bit (Sony)
    uint32_t frequency;
    uint8_t  duty;
};

constexpr PdwmSpec kNecSpec{9000, 4500, 560, 1690, 560, 560, false, 38000, 33};
constexpr PdwmSpec kSamsungSpec{4500, 4500, 560, 1690, 560, 560, false, 38000, 33};
constexpr PdwmSpec kSircSpec{2400, 600, 1200, 600, 600, 600, true, 40000, 33};
constexpr PdwmSpec kKaseikyoSpec{3456, 1728, 432, 1296, 432, 432, false, 37000, 33};

// Gree / YAPOF.  Same 38 kHz leader as NEC but a different payload shape: two
// 4-byte blocks separated by a 3-bit marker (0b010) and a ~20 ms gap, and a
// trailing gap after the last block.  Numbers from IRremoteESP8266's ir_Gree.cpp.
constexpr uint16_t kGreeHdrMark   = 9000;
constexpr uint16_t kGreeHdrSpace  = 4500;
constexpr uint16_t kGreeBitMark   = 620;
constexpr uint16_t kGreeOneSpace  = 1600;
constexpr uint16_t kGreeZeroSpace = 540;
constexpr uint16_t kGreeMsgSpace  = 19980;
constexpr int kGreeBits           = 64;
constexpr int kGreeTotalBits      = kGreeBits + 3;  // payload + the 0b010 marker

// `data` is LSB-first across bytes, exactly like Flipper's encoder walks it.
bool encodePdwm(Timings* out, const uint8_t* data, int nbits, const PdwmSpec& spec)
{
    Enc e(out);
    if (spec.preambleMark != 0) {
        e.add(true, spec.preambleMark);
        e.add(false, spec.preambleSpace);
    }
    for (int i = 0; i < nbits; ++i) {
        const bool bit = (data[i >> 3] >> (i & 7)) & 1;
        e.add(true, bit ? spec.bit1Mark : spec.bit0Mark);
        // A mark-encoded protocol ends on the last mark; a space-encoded one
        // adds a stop mark after its last space, which is why the loop does not
        // simply append a trailer for both.
        if (!(spec.markEncoded && i == nbits - 1)) {
            e.add(false, bit ? spec.bit1Space : spec.bit0Space);
        }
    }
    if (!spec.markEncoded) e.add(true, spec.bit1Mark);
    return e.finish(spec.frequency, spec.duty);
}

// ---------------------------------------------------------------------------
// Manchester encoder
//
// `cells` holds one entry per bit; the whole bit is emitted as two half-bit
// runs.  `markFirst` selects the polarity (see the file comment) and
// `doubleBit` is the index whose pair is twice as wide (the RC6 toggle field).
// ---------------------------------------------------------------------------
bool encodeManchester(Timings* out, const bool* cells, int nbits, uint16_t preambleMark,
                      uint16_t preambleSpace, uint16_t cell, int doubleBit, uint32_t frequency,
                      uint8_t duty)
{
    Enc e(out);
    if (preambleMark != 0) {
        e.add(true, preambleMark);
        e.add(false, preambleSpace);
    }
    for (int i = 0; i < nbits; ++i) {
        const uint32_t width = cell * ((i == doubleBit) ? 2u : 1u);
        e.add(cells[i], width);
        e.add(!cells[i], width);
    }
    return e.finish(frequency, duty);
}

inline bool bitOf(const uint8_t* data, int i) { return ((data[i >> 3] >> (i & 7)) & 1) != 0; }
inline void putBit(uint8_t* data, int i, bool v)
{
    if (v) data[i >> 3] = static_cast<uint8_t>(data[i >> 3] | (1u << (i & 7)));
}

// ---------------------------------------------------------------------------
// Gree / YAPOF air conditioner
// ---------------------------------------------------------------------------
// Defined with the other decode helpers further down; the Gree decoder is
// written before them so the encoder and decoder of one protocol stay together.
inline bool near(uint32_t got, uint32_t want, uint32_t tol);

bool encodeGree(const Signal& s, Timings* out)
{
    uint8_t b[8];
    for (int i = 0; i < 4; ++i) {
        b[i]     = static_cast<uint8_t>((s.address >> (8 * i)) & 0xFF);
        b[i + 4] = static_cast<uint8_t>((s.command >> (8 * i)) & 0xFF);
    }

    Enc e(out);
    e.add(true, kGreeHdrMark);
    e.add(false, kGreeHdrSpace);

    auto emitByte = [&e](uint8_t value) {
        for (int bit = 0; bit < 8; ++bit) {  // LSB first, like every Gree block
            const bool one = ((value >> bit) & 1) != 0;
            e.add(true, kGreeBitMark);
            e.add(false, one ? kGreeOneSpace : kGreeZeroSpace);
        }
    };

    for (int i = 0; i < 4; ++i) emitByte(b[i]);

    // Mid-frame marker 0b010 (LSB first) and the long gap that separates the two
    // halves of the state.
    for (int bit = 0; bit < 3; ++bit) {
        const bool one = ((0x2 >> bit) & 1) != 0;
        e.add(true, kGreeBitMark);
        e.add(false, one ? kGreeOneSpace : kGreeZeroSpace);
    }
    e.add(true, kGreeBitMark);
    e.add(false, kGreeMsgSpace);

    for (int i = 4; i < 8; ++i) emitByte(b[i]);

    // Trailing gap: the air conditioner wants silence after the last block, and
    // a real remote leaves ~40 ms here (see the GREE_YAPOF capture).
    e.add(true, kGreeBitMark);
    e.add(false, kGreeMsgSpace);
    return e.finish(38000, 33);
}

// Walks the (mark, space) pairs after the leader.  Runs wider than ~5 ms are the
// two gap markers, not bits; everything else must be a 620 us mark followed by a
// 1600 us (1) or 540 us (0) space.
bool decodeGree(const uint16_t* us, int count, Signal* out)
{
    bool payload[kGreeTotalBits];
    int n = 0;

    for (int i = 2; i + 1 < count && n < kGreeTotalBits; i += 2) {
        const uint32_t mark  = us[i];
        const uint32_t space = us[i + 1];
        if (space >= 5000) continue;  // 19980 us gap marker
        if (!near(mark, kGreeBitMark, 260)) return false;
        if (near(space, kGreeOneSpace, 420)) {
            payload[n++] = true;
        } else if (near(space, kGreeZeroSpace, 260)) {
            payload[n++] = false;
        } else {
            return false;
        }
    }
    if (n < kGreeTotalBits) return false;

    // The 3-bit marker between the blocks is 0b010, LSB first.
    const bool marker[3] = {false, true, false};
    for (int i = 0; i < 3; ++i) {
        if (payload[32 + i] != marker[i]) return false;
    }

    uint8_t b[8] = {0};
    // payload[0..31] is the first block, [32..34] the 0b010 marker, [35..66] the
    // second block.  The marker is dropped when re-packing the 64 bits.
    for (int i = 0; i < 32; ++i) putBit(b, i, payload[i]);
    for (int i = 0; i < 32; ++i) putBit(b, 32 + i, payload[35 + i]);
    if (gree::checksum(b, 8) != static_cast<uint8_t>(b[7] >> 4)) return false;

    uint32_t address = 0, command = 0;
    for (int i = 0; i < 4; ++i) {
        address |= static_cast<uint32_t>(b[i]) << (8 * i);
        command |= static_cast<uint32_t>(b[i + 4]) << (8 * i);
    }

    out->proto   = Protocol::kGree;
    out->address = address;
    out->command = command;
    return true;
}

// ---------------------------------------------------------------------------
// Decode helpers
// ---------------------------------------------------------------------------
inline bool near(uint32_t got, uint32_t want, uint32_t tol)
{
    return got + tol >= want && got <= want + tol;
}

bool startsWith(const uint16_t* us, uint32_t m, uint32_t s, uint32_t tol)
{
    return near(us[0], m, tol) && near(us[1], s, tol);
}

// PDWM payload: us[0..1] is the preamble, then (mark, space) per bit.
//
// The number of bits depends on which half carries the value:
//   space-encoded (NEC family, Kaseikyo): mark + space per bit, then a stop mark
//       -> count = 2 * bits + 3
//   mark-encoded (Sony): mark + space per bit, no stop mark
//       -> count = 2 * bits + 1
// Returns the number of bits recovered, or -1.
int decodePdwmPayload(const uint16_t* us, int count, const PdwmSpec& spec, uint8_t* data, int maxBytes,
                      uint32_t tol)
{
    const int avail = count - 2;
    if (avail < 2) return -1;

    const int bits = spec.markEncoded ? (avail + 1) / 2 : avail / 2;
    if (bits <= 0 || bits > maxBytes * 8) return -1;
    std::memset(data, 0, static_cast<size_t>(maxBytes));

    for (int i = 0; i < bits; ++i) {
        const int markIndex = 2 + 2 * i;
        if (markIndex >= count) return -1;
        const uint32_t mark = us[markIndex];

        bool value;
        if (spec.markEncoded) {
            if (near(mark, spec.bit1Mark, tol)) value = true;
            else if (near(mark, spec.bit0Mark, tol)) value = false;
            else return -1;
        } else {
            const int spaceIndex = markIndex + 1;
            if (spaceIndex >= count) return -1;  // a cut-off last bit
            const uint32_t space = us[spaceIndex];
            if (!near(mark, spec.bit0Mark, tol)) return -1;
            if (near(space, spec.bit1Space, tol)) value = true;
            else if (near(space, spec.bit0Space, tol)) value = false;
            else return -1;
        }
        putBit(data, i, value);
    }
    return bits;
}

// Expand alternating runs into one entry per 888/444 us cell.  `levels[i]` is
// true for a mark.  Runs wider than four cells end the frame (that is the
// inter-frame gap).  Returns the number of cells recovered.
int expandCells(const uint16_t* us, int count, uint32_t cell, bool* levels, int maxCells)
{
    int n        = 0;
    bool isMark  = true;  // ir::Timings always start with a mark
    for (int i = 0; i < count && n < maxCells; ++i) {
        const uint32_t units = (us[i] + cell / 2) / cell;
        if (units == 0) return n;          // noise
        if (units > 4) return n;           // gap: frame ends
        for (uint32_t u = 0; u < units && n < maxCells; ++u) levels[n++] = isMark;
        isMark = !isMark;
    }
    return n;
}

// RC6: 21 bits.  Bits 0..3 are 444 us Manchester cells, the toggle bit (index
// 4) is a *double* bit -- both halves 888 us, which expands to four 444 cells
// of pattern a,a,!a,!a -- and bits 5..20 are ordinary cells again.
bool decodeRc6(const uint16_t* us, int count, Signal* out)
{
    bool levels[64];
    const int cells = expandCells(us + 2, count - 2, 444, levels, 64);
    // 4 bits (8 cells) + the double toggle (4 cells) + 16 bits (32 cells).
    if (cells < 44) return false;

    bool bits[21];
    for (int i = 0; i < 4; ++i) {
        if (levels[2 * i] == levels[2 * i + 1]) return false;
        bits[i] = levels[2 * i];
    }
    // Toggle field: half 1 spans cells 8..9, half 2 spans 10..11.
    if (levels[8] != levels[9] || levels[10] != levels[11]) return false;
    if (levels[8] == levels[10]) return false;
    bits[4] = levels[8];
    for (int i = 5; i < 21; ++i) {
        const int at = 2 * i + 2;
        if (levels[at] == levels[at + 1]) return false;
        bits[i] = levels[at];
    }
    if (!bits[0]) return false;                       // start bit
    if (bits[1] || bits[2] || bits[3]) return false;  // mode 0

    uint32_t address = 0, command = 0;
    for (int i = 0; i < 8; ++i) address = (address << 1) | (bits[5 + i] ? 1u : 0u);
    for (int i = 0; i < 8; ++i) command = (command << 1) | (bits[13 + i] ? 1u : 0u);

    out->proto   = Protocol::kRc6;
    out->address = address;
    out->command = command;
    out->toggle  = bits[4];
    return true;
}

// RC5 / RC5X: the reference polarity starts the frame with a space, and
// ir::Timings start with a mark, so the first mark in hand is the *second* half
// of the start bit.  The start bit is always 1, so that mark is also proof the
// capture is RC5-shaped.
//
// RC5 and RC5X differ only in the second start bit (1 on RC5, 0 on RC5X, where
// it introduces a seventh command bit), so both lengths are tried and the
// protocol is identified by that bit rather than guessed from the run count.
bool decodeRc5(const uint16_t* us, int count, Signal* out)
{
    bool levels[48];
    const int cells = expandCells(us, count, 888, levels, 48);
    if (cells < 26) return false;
    if (!levels[0]) return false;  // RC5 start bit "1" ends in a mark

    for (int nbits = 14; nbits <= 15; ++nbits) {
        // Bit 0 contributes one cell (its leading half space was dropped).
        if (cells < 2 * nbits - 2) continue;

        bool bits[15];
        bool ok = true;
        bits[0] = true;
        for (int i = 1; i < nbits; ++i) {
            const int first = 2 * i - 1;  // see the offset above
            if (first + 1 >= cells) {
                ok = false;
                break;
            }
            if (levels[first] == levels[first + 1]) {
                ok = false;
                break;
            }
            bits[i] = !levels[first];  // RC5: a logical 1 begins with a space
        }
        if (!ok) continue;
        if (bits[1] != (nbits == 14)) continue;  // RC5: 1, RC5X: 0

        uint32_t address = 0, command = 0;
        for (int i = 0; i < 5; ++i) address = (address << 1) | (bits[3 + i] ? 1u : 0u);
        const int cmdBits = nbits - 8;
        for (int i = 0; i < cmdBits; ++i) command = (command << 1) | (bits[8 + i] ? 1u : 0u);

        out->proto   = nbits == 15 ? Protocol::kRc5X : Protocol::kRc5;
        out->address = address;
        out->command = command;
        out->toggle  = bits[2];
        return true;
    }
    return false;
}

bool decodeNec(const uint16_t* us, int count, Signal* out)
{
    uint8_t d[4];
    if (decodePdwmPayload(us, count, kNecSpec, d, 4, 350) != 32) return false;

    const uint8_t a0 = d[0], a1 = d[1], c0 = d[2], c1 = d[3];
    if (static_cast<uint8_t>(a0 ^ a1) == 0xFF && static_cast<uint8_t>(c0 ^ c1) == 0xFF) {
        out->proto   = Protocol::kNec;
        out->address = a0;
        out->command = c0;
    } else {
        out->proto   = Protocol::kNecExt;
        out->address = static_cast<uint32_t>(a0) | (static_cast<uint32_t>(a1) << 8);
        out->command = static_cast<uint32_t>(c0) | (static_cast<uint32_t>(c1) << 8);
    }
    return true;
}

bool decodeSamsung(const uint16_t* us, int count, Signal* out)
{
    uint8_t d[4];
    if (decodePdwmPayload(us, count, kSamsungSpec, d, 4, 350) != 32) return false;
    if (d[0] != d[1]) return false;                                    // duplicated address
    if (static_cast<uint8_t>(d[2] ^ d[3]) != 0xFF) return false;       // command + complement
    out->proto   = Protocol::kSamsung32;
    out->address = d[0];
    out->command = d[2];
    return true;
}

bool decodeSirc(const uint16_t* us, int count, Signal* out)
{
    uint8_t d[4];
    const int bits = decodePdwmPayload(us, count, kSircSpec, d, 4, 300);
    if (bits != 12 && bits != 15 && bits != 20) return false;

    uint32_t address = 0, command = 0;
    for (int i = 0; i < 7; ++i) command |= static_cast<uint32_t>(bitOf(d, i)) << i;
    for (int i = 0; i < bits - 7; ++i) address |= static_cast<uint32_t>(bitOf(d, 7 + i)) << i;

    out->proto   = bits == 12 ? Protocol::kSirc : (bits == 15 ? Protocol::kSirc15 : Protocol::kSirc20);
    out->address = address;
    out->command = command;
    return true;
}

bool decodeKaseikyo(const uint16_t* us, int count, Signal* out)
{
    uint8_t d[8];
    if (decodePdwmPayload(us, count, kKaseikyoSpec, d, 8, 220) != 48) return false;
    if (static_cast<uint8_t>(d[2] ^ d[3] ^ d[4]) != d[5]) return false;

    const uint8_t genre1 = static_cast<uint8_t>(d[2] >> 4);
    const uint8_t genre2 = static_cast<uint8_t>(d[3] & 0xF);
    const uint8_t id     = static_cast<uint8_t>(d[4] >> 6);
    const uint16_t vendor = static_cast<uint16_t>(d[0] | (d[1] << 8));
    const uint16_t command =
        static_cast<uint16_t>((d[3] >> 4) | (static_cast<uint16_t>(d[4] & 0x3F) << 4));

    out->proto   = Protocol::kKaseikyo;
    out->address = (static_cast<uint32_t>(id) << 24) | (static_cast<uint32_t>(vendor) << 8) |
                   (static_cast<uint32_t>(genre1) << 4) | genre2;
    out->command = command;
    return true;
}

// ---------------------------------------------------------------------------
// Name table
// ---------------------------------------------------------------------------
struct ProtoInfo {
    Protocol proto;
    const char* name;
    uint32_t carrier;
};

const ProtoInfo kProtos[] = {
    {Protocol::kRaw, "RAW", 38000},
    {Protocol::kNec, "NEC", 38000},
    {Protocol::kNecExt, "NECext", 38000},
    {Protocol::kSamsung32, "Samsung32", 38000},
    {Protocol::kRc5, "RC5", 36000},
    {Protocol::kRc5X, "RC5X", 36000},
    {Protocol::kRc6, "RC6", 36000},
    {Protocol::kSirc, "SIRC", 40000},
    {Protocol::kSirc15, "SIRC15", 40000},
    {Protocol::kSirc20, "SIRC20", 40000},
    {Protocol::kKaseikyo, "Kaseikyo", 37000},
    {Protocol::kGree, "Gree", 38000},
};

const ProtoInfo* info(Protocol p)
{
    for (const ProtoInfo& i : kProtos) {
        if (i.proto == p) return &i;
    }
    return &kProtos[0];
}

bool sameName(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) return false;
    while (*a != 0 && *b != 0) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

}  // namespace

const char* protocolName(Protocol p)
{
    if (p == Protocol::kUnknown) return "UNKNOWN";
    return info(p)->name;
}

Protocol protocolFromName(const char* name)
{
    if (name == nullptr || name[0] == 0) return Protocol::kUnknown;
    for (const ProtoInfo& i : kProtos) {
        if (sameName(i.name, name)) return i.proto;
    }
    return Protocol::kUnknown;
}

uint32_t carrierHz(Protocol p) { return info(p)->carrier; }

bool encodable(Protocol p)
{
    switch (p) {
        case Protocol::kNec:
        case Protocol::kNecExt:
        case Protocol::kSamsung32:
        case Protocol::kRc5:
        case Protocol::kRc5X:
        case Protocol::kRc6:
        case Protocol::kSirc:
        case Protocol::kSirc15:
        case Protocol::kSirc20:
        case Protocol::kKaseikyo: return true;
        case Protocol::kGree: return true;
        default: return false;
    }
}

bool protocolSupported(const char* name)
{
    const Protocol p = protocolFromName(name);
    return p != Protocol::kUnknown && p != Protocol::kRaw && encodable(p);
}

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------
namespace {

bool encodeNec(const Signal& s, Timings* out)
{
    uint8_t d[4];
    if (s.proto == Protocol::kNec) {
        const uint8_t addr = static_cast<uint8_t>(s.address & 0xFF);
        const uint8_t cmd  = static_cast<uint8_t>(s.command & 0xFF);
        d[0] = addr;
        d[1] = static_cast<uint8_t>(~addr);
        d[2] = cmd;
        d[3] = static_cast<uint8_t>(~cmd);
    } else {  // NECext: no complement bytes, 16 bit address and 16 bit command
        d[0] = static_cast<uint8_t>(s.address & 0xFF);
        d[1] = static_cast<uint8_t>((s.address >> 8) & 0xFF);
        d[2] = static_cast<uint8_t>(s.command & 0xFF);
        d[3] = static_cast<uint8_t>((s.command >> 8) & 0xFF);
    }
    return encodePdwm(out, d, 32, kNecSpec);
}

bool encodeSamsung(const Signal& s, Timings* out)
{
    const uint8_t addr = static_cast<uint8_t>(s.address & 0xFF);
    const uint8_t cmd  = static_cast<uint8_t>(s.command & 0xFF);
    const uint8_t d[4] = {addr, addr, cmd, static_cast<uint8_t>(~cmd)};
    return encodePdwm(out, d, 32, kSamsungSpec);
}

bool encodeSirc(const Signal& s, Timings* out)
{
    const int nbits    = s.proto == Protocol::kSirc ? 12 : (s.proto == Protocol::kSirc15 ? 15 : 20);
    const int addrBits = nbits - 7;
    uint8_t d[4]       = {0, 0, 0, 0};
    for (int i = 0; i < 7; ++i) putBit(d, i, ((s.command >> i) & 1) != 0);  // command first
    for (int i = 0; i < addrBits; ++i) putBit(d, 7 + i, ((s.address >> i) & 1) != 0);
    return encodePdwm(out, d, nbits, kSircSpec);
}

bool encodeKaseikyo(const Signal& s, Timings* out)
{
    const uint32_t address = s.address;
    const uint16_t command = static_cast<uint16_t>(s.command & 0x3FF);

    const uint8_t id      = static_cast<uint8_t>((address >> 24) & 3);
    const uint16_t vendor = static_cast<uint16_t>((address >> 8) & 0xFFFF);
    const uint8_t genre1  = static_cast<uint8_t>((address >> 4) & 0xF);
    const uint8_t genre2  = static_cast<uint8_t>(address & 0xF);

    uint8_t d[6];
    d[0] = static_cast<uint8_t>(vendor & 0xFF);
    d[1] = static_cast<uint8_t>(vendor >> 8);
    uint8_t parity = static_cast<uint8_t>(d[0] ^ d[1]);
    parity         = static_cast<uint8_t>((parity & 0xF) ^ (parity >> 4));
    d[2] = static_cast<uint8_t>((parity & 0xF) | (genre1 << 4));
    d[3] = static_cast<uint8_t>((genre2 & 0xF) | ((command & 0xF) << 4));
    d[4] = static_cast<uint8_t>((id << 6) | (command >> 4));
    d[5] = static_cast<uint8_t>(d[2] ^ d[3] ^ d[4]);
    return encodePdwm(out, d, 48, kKaseikyoSpec);
}

// RC5 / RC5X.  Cells are MSB first: start, second start (0 on RC5X -- that bit
// is where its seventh command bit lives), toggle, 5 address, 6 or 7 command.
// The payload is inverted before Manchester so a logical 1 emits
// space-then-mark, matching the reference encoder.
bool encodeRc5(const Signal& s, Timings* out)
{
    const bool extended = s.proto == Protocol::kRc5X;
    const int cmdBits   = extended ? 7 : 6;

    bool cells[15];
    int n = 0;
    cells[n++] = true;       // start bit, always 1
    cells[n++] = !extended;  // RC5: 1, RC5X: 0
    cells[n++] = s.toggle;   // flips on every press
    for (int i = 4; i >= 0; --i) cells[n++] = ((s.address >> i) & 1) != 0;
    for (int i = cmdBits - 1; i >= 0; --i) cells[n++] = ((s.command >> i) & 1) != 0;

    for (int i = 0; i < n; ++i) cells[i] = !cells[i];
    return encodeManchester(out, cells, n, 0, 0, 888, -1, 36000, 33);
}

// RC6 mode 0.  21 cells: start=1, three mode bits (0), toggle, 8 address,
// 8 command, all MSB first; the toggle pair is twice as wide.
bool encodeRc6(const Signal& s, Timings* out)
{
    bool cells[21];
    int n = 0;
    cells[n++] = true;   // start
    cells[n++] = false;  // mode 0
    cells[n++] = false;
    cells[n++] = false;
    cells[n++] = s.toggle;  // doubled width
    for (int i = 7; i >= 0; --i) cells[n++] = ((s.address >> i) & 1) != 0;
    for (int i = 7; i >= 0; --i) cells[n++] = ((s.command >> i) & 1) != 0;
    return encodeManchester(out, cells, n, 2666, 889, 444, 4, 36000, 33);
}

}  // namespace

bool encode(const Signal& s, Timings* out)
{
    if (out == nullptr) return false;
    out->count = 0;
    if (!encodable(s.proto)) return false;

    switch (s.proto) {
        case Protocol::kNec:
        case Protocol::kNecExt: return encodeNec(s, out);
        case Protocol::kSamsung32: return encodeSamsung(s, out);
        case Protocol::kRc5:
        case Protocol::kRc5X: return encodeRc5(s, out);
        case Protocol::kRc6: return encodeRc6(s, out);
        case Protocol::kSirc:
        case Protocol::kSirc15:
        case Protocol::kSirc20: return encodeSirc(s, out);
        case Protocol::kKaseikyo: return encodeKaseikyo(s, out);
        case Protocol::kGree: return encodeGree(s, out);
        default: return false;
    }
}

bool encodeRepeat(const Signal& s, Timings* out)
{
    if (out == nullptr) return false;
    // Only the NEC family has a dedicated short repeat frame; everything else
    // repeats the whole payload.
    if (s.proto == Protocol::kNec || s.proto == Protocol::kNecExt) {
        Enc e(out);
        e.add(true, 9000);
        e.add(false, 2250);
        e.add(true, 560);
        return e.finish(38000, 33);
    }
    return encode(s, out);
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------
bool decode(const uint16_t* us, int count, Signal* out)
{
    if (us == nullptr || out == nullptr || count < 4) return false;

    out->proto   = Protocol::kUnknown;
    out->address = 0;
    out->command = 0;
    out->toggle  = false;
    out->repeat  = false;

    // Order matters: RC6's 2666/889 leader also lands inside a generous Sony
    // preamble window, so the more specific protocol is tested first.
    if (startsWith(us, 9000, 4500, 1600) && decodeNec(us, count, out)) return true;
    // Gree shares NEC's leader, so this must come after the NEC attempt: a Gree
    // frame is 67 bits long and the NEC decoder rejects anything that is not 32.
    if (startsWith(us, kGreeHdrMark, kGreeHdrSpace, 1600) && decodeGree(us, count, out)) return true;
    if (startsWith(us, 4500, 4500, 900) && decodeSamsung(us, count, out)) return true;
    if (startsWith(us, 2666, 889, 400) && decodeRc6(us, count, out)) return true;
    if (startsWith(us, 3456, 1728, 700) && decodeKaseikyo(us, count, out)) return true;
    if (startsWith(us, 2400, 600, 240) && decodeSirc(us, count, out)) return true;
    if (decodeRc5(us, count, out)) return true;

    out->proto = Protocol::kUnknown;
    return false;
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------
int describe(const Signal& s, char* out, int cap)
{
    if (out == nullptr || cap <= 0) return 0;
    if (s.proto == Protocol::kUnknown) {
        return std::snprintf(out, static_cast<size_t>(cap), "UNKNOWN");
    }
    if (s.proto == Protocol::kRaw) return std::snprintf(out, static_cast<size_t>(cap), "RAW");
    return std::snprintf(out, static_cast<size_t>(cap), "%s A=%lX C=%lX", protocolName(s.proto),
                         static_cast<unsigned long>(s.address),
                         static_cast<unsigned long>(s.command));
}

// ---------------------------------------------------------------------------
// Gree / YAPOF air conditioner state
//
// Byte layout (IRremoteESP8266's GreeProtocol union, bit 0 = LSB):
//   0: Mode:3  Power:1  Fan:2  SwingAuto:1  Sleep:1
//   1: Temp:4  TimerHalfHr:1  TimerTensHr:2  TimerEnabled:1
//   2: TimerHours:4  Turbo:1  Light:1  ModelA:1  Xfan:1
//   3: (2 spare) TempExtraDegreeF:1 UseFahrenheit:1  unknown1:4 = 0b0101
//   4: SwingV:4  SwingH:3  (1 spare)
//   5: DisplayTemp:2  IFeel:1  unknown2:3 = 0b100  WiFi:1  (1 spare)
//   6: timer minutes, 0 without a timer
//   7: (2 spare) Econo:1 (1 spare) Sum:4
//
// `unknown1` and `unknown2` are not decoration: a real remote sends 5 and 4
// there every time, and the unit ignores frames that do not.
// ---------------------------------------------------------------------------
namespace gree {
namespace {

constexpr uint8_t kChecksumStart = 10;  // Kelvinator's start value

uint8_t clampTemp(uint8_t tempC)
{
    if (tempC < kMinTempC) return kMinTempC;
    if (tempC > kMaxTempC) return kMaxTempC;
    return tempC;
}

}  // namespace

uint8_t checksum(const uint8_t* raw, int len)
{
    if (raw == nullptr || len < 2) return 0;
    uint8_t sum = kChecksumStart;
    for (int i = 0; i < 4 && i < len - 1; ++i) sum = static_cast<uint8_t>(sum + (raw[i] & 0x0F));
    for (int i = 4; i < len - 1; ++i) sum = static_cast<uint8_t>(sum + (raw[i] >> 4));
    return static_cast<uint8_t>(sum & 0x0F);
}

State& fixup(State& s)
{
    s.raw[7] = static_cast<uint8_t>((s.raw[7] & 0x0F) | (checksum(s.raw) << 4));
    return s;
}

bool valid(const State& s) { return checksum(s.raw) == static_cast<uint8_t>(s.raw[7] >> 4); }

State makeState(bool power, uint8_t mode, uint8_t tempC, uint8_t fan, bool swing, bool light,
                uint8_t model)
{
    if (mode > kModeHeat) mode = kModeCool;
    if (fan > kFanMax) fan = kFanAuto;

    State s;
    // Byte 0
    s.raw[0] = static_cast<uint8_t>((mode & 0x7) | ((power ? 1u : 0u) << 3) | ((fan & 0x3) << 4) |
                                    ((swing ? 1u : 0u) << 6));
    // Byte 1: temperature is stored as an offset from 16 C.
    s.raw[1] = static_cast<uint8_t>(clampTemp(tempC) - kMinTempC);
    // Byte 2: Light, plus the "model A" bit that the YAW1F family sets only
    // while the unit is running.
    const bool modelA = (model == kModelYaw1F) && power;
    s.raw[2] = static_cast<uint8_t>((light ? 0x20 : 0x00) | (modelA ? 0x40 : 0x00));
    // Byte 3: the fixed 0b0101 the protocol requires.
    s.raw[3] = 0x50;
    // Byte 4: vane position; SwingAuto already lives in byte 0.
    s.raw[4] = static_cast<uint8_t>(swing ? 0x01 : 0x00);
    // Byte 5: the second family marker.  YAW1F carries 0b100 in bits 3..5,
    // YAPOF carries 0 and sets the WiFi bit instead.
    s.raw[5] = (model == kModelYaw1F) ? 0x20 : 0x40;
    s.raw[6] = 0x00;
    return fixup(s);
}

Signal toSignal(const State& s)
{
    Signal out;
    out.proto = Protocol::kGree;
    for (int i = 0; i < 4; ++i) {
        out.address |= static_cast<uint32_t>(s.raw[i]) << (8 * i);
        out.command |= static_cast<uint32_t>(s.raw[i + 4]) << (8 * i);
    }
    return out;
}

bool fromSignal(const Signal& signal, State* out)
{
    if (out == nullptr || signal.proto != Protocol::kGree) return false;
    for (int i = 0; i < 4; ++i) {
        out->raw[i]     = static_cast<uint8_t>((signal.address >> (8 * i)) & 0xFF);
        out->raw[i + 4] = static_cast<uint8_t>((signal.command >> (8 * i)) & 0xFF);
    }
    return true;
}

const char* modeName(uint8_t mode)
{
    switch (mode) {
        case kModeAuto: return "AUTO";
        case kModeCool: return "COOL";
        case kModeDry: return "DRY";
        case kModeFan: return "FAN";
        case kModeHeat: return "HEAT";
        default: return "?";
    }
}

const char* fanName(uint8_t fan)
{
    switch (fan) {
        case kFanAuto: return "AUTO";
        case kFanMin: return "LOW";
        case kFanMed: return "MID";
        case kFanMax: return "HIGH";
        default: return "?";
    }
}

}  // namespace gree

}  // namespace ir
