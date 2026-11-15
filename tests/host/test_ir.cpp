// Host tests for the IR protocol engine (src/ir_protocol.cpp).
//
//   .\tests\host\run.ps1
//
// The engine is pure C++, so the whole encode/decode path runs here in
// milliseconds instead of after a cross-compile to the board.  What these
// tests cannot tell you is whether the carrier lands on 38.0 kHz or whether
// the LED is bright enough -- that needs the hardware.

#include <cstdio>
#include <cstring>

#include "ir_protocol.h"

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL %d: %s\n", line, what);
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const long long va = static_cast<long long>(a);                             \
        const long long vb = static_cast<long long>(b);                             \
        ++g_checks;                                                                 \
        if (va != vb) {                                                             \
            ++g_failures;                                                           \
            std::printf("FAIL %d: %s == %s (%lld vs %lld)\n", __LINE__, #a, #b, va, vb); \
        }                                                                           \
    } while (false)

// A frame must alternate mark/space, start with a mark and end with a mark.
bool wellFormed(const ir::Timings& t)
{
    if (t.count < 4) return false;
    if (t.count > ir::kMaxTimings) return false;
    for (int i = 0; i < t.count; ++i) {
        if (t.us[i] == 0) return false;
    }
    return true;  // count odd means it ends on a mark
}

// ---------------------------------------------------------------------------
// NEC
// ---------------------------------------------------------------------------
void testNec()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kNec;
    s.address = 0x04;
    s.command = 0x08;

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    CHECK(wellFormed(t));
    CHECK_EQ(t.count, 67);         // 9000, 4500, 32 x (mark, space), stop mark
    CHECK_EQ(t.us[0], 9000);
    CHECK_EQ(t.us[1], 4500);
    CHECK_EQ(t.us[t.count - 1], 560);
    CHECK_EQ(t.frequency, 38000);

    // address 0x04 = 0000 0100 LSB first -> bit 2 is the first "1"
    // bits 0..7 = 0,0,1,0,0,0,0,0 ; the first entries come from the address LSBs.
    CHECK_EQ(t.us[2], 560);   // bit 0 mark
    CHECK_EQ(t.us[3], 560);   // bit 0 space: 0
    CHECK_EQ(t.us[6], 560);   // bit 2 mark
    CHECK_EQ(t.us[7], 1690);  // bit 2 space: 1

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kNec);
    CHECK_EQ(back.address, 0x04);
    CHECK_EQ(back.command, 0x08);
}

void testNecRepeatFrame()
{
    ir::Signal s;
    s.proto = ir::Protocol::kNec;
    ir::Timings t;
    CHECK(ir::encodeRepeat(s, &t));
    CHECK_EQ(t.count, 3);
    CHECK_EQ(t.us[0], 9000);
    CHECK_EQ(t.us[1], 2250);
    CHECK_EQ(t.us[2], 560);
}

void testNecExt()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kNecExt;
    s.address = 0x87EE;  // the Flipper documentation example
    s.command = 0xA05D;

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    CHECK_EQ(t.count, 67);  // same frame shape as NEC

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kNecExt);
    CHECK_EQ(back.address, 0x87EE);
    CHECK_EQ(back.command, 0xA05D);
}

// ---------------------------------------------------------------------------
// Samsung32
// ---------------------------------------------------------------------------
void testSamsung()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kSamsung32;
    s.address = 0x07;
    s.command = 0x02;

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    CHECK_EQ(t.us[0], 4500);
    CHECK_EQ(t.us[1], 4500);
    CHECK_EQ(t.count, 67);
    CHECK_EQ(t.frequency, 38000);

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kSamsung32);
    CHECK_EQ(back.address, 0x07);
    CHECK_EQ(back.command, 0x02);
}

// ---------------------------------------------------------------------------
// Sony SIRC: mark encoded, 40 kHz
// ---------------------------------------------------------------------------
void testSirc()
{
    for (int i = 0; i < 3; ++i) {
        ir::Signal s;
        s.proto   = i == 0 ? ir::Protocol::kSirc : (i == 1 ? ir::Protocol::kSirc15 : ir::Protocol::kSirc20);
        s.address = i == 2 ? 0x1234 & 0x1FFF : (i == 1 ? 0x9A : 0x01);
        s.command = 0x15;

        ir::Timings t;
        CHECK(ir::encode(s, &t));
        CHECK_EQ(t.us[0], 2400);
        CHECK_EQ(t.us[1], 600);
        CHECK_EQ(t.frequency, 40000);
        // command 0x15 LSB first = 1,0,1,0,1,0,0 -> first bit is a 1 (1200 mark)
        CHECK_EQ(t.us[2], 1200);

        ir::Signal back;
        CHECK(ir::decode(t.us, t.count, &back));
        CHECK(back.proto == s.proto);
        CHECK_EQ(back.address, s.address);
        CHECK_EQ(back.command, s.command);
    }
}

// ---------------------------------------------------------------------------
// RC5 / RC5X / RC6: Manchester, 36 kHz
// ---------------------------------------------------------------------------
void testRc5()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kRc5;
    s.address = 0x05;
    s.command = 0x35;
    s.toggle  = false;

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    CHECK(wellFormed(t));
    CHECK_EQ(t.frequency, 36000);
    // 14 bits x 2 cells = 28 cells; the leading space is dropped and adjacent
    // equal cells merge, so the frame is shorter than 28 entries.
    CHECK(t.count >= 14 && t.count <= 27);

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kRc5);
    CHECK_EQ(back.address, 0x05);
    CHECK_EQ(back.command, 0x35);
    CHECK_EQ(back.toggle, 0);

    // A toggle press must produce a different waveform and decode back as 1.
    s.toggle = true;
    ir::Timings t2;
    CHECK(ir::encode(s, &t2));
    CHECK(t2.count == t.count);  // same length, different middle
    ir::Signal back2;
    CHECK(ir::decode(t2.us, t2.count, &back2));
    CHECK_EQ(back2.toggle, 1);
}

void testRc6()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kRc6;
    s.address = 0x00;
    s.command = 0x0C;  // Philips "power"
    s.toggle  = false;

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    CHECK_EQ(t.us[0], 2666);
    CHECK_EQ(t.us[1], 889);

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kRc6);
    CHECK_EQ(back.address, 0x00);
    CHECK_EQ(back.command, 0x0C);
}

// ---------------------------------------------------------------------------
// Kaseikyo (Panasonic)
// ---------------------------------------------------------------------------
void testKaseikyo()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kKaseikyo;
    s.address = 0x00020221;  // vendor 0x0002, genre1 2, genre2 1 (26 bit field)
    s.command = 0x3D0;

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    CHECK_EQ(t.us[0], 3456);
    CHECK_EQ(t.us[1], 1728);
    CHECK_EQ(t.frequency, 37000);

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kKaseikyo);
    CHECK_EQ(back.address, s.address);
    CHECK_EQ(back.command, s.command);
}

// ---------------------------------------------------------------------------
// Robustness
// ---------------------------------------------------------------------------
void testRejects()
{
    ir::Timings t;
    ir::Signal s;

    s.proto = ir::Protocol::kRaw;
    CHECK(!ir::encode(s, &t));
    s.proto = ir::Protocol::kUnknown;
    CHECK(!ir::encode(s, &t));

    // A raw capture of nothing must not be mistaken for a protocol.
    static const uint16_t garbage[] = {100, 200, 300, 400, 500, 600, 700, 800};
    CHECK(!ir::decode(garbage, 8, &s));
    CHECK(s.proto == ir::Protocol::kUnknown);
    CHECK(!ir::decode(garbage, 2, &s));

    // A truncated NEC frame (preamble only) must fail cleanly.
    static const uint16_t truncated[] = {9000, 4500, 560};
    CHECK(!ir::decode(truncated, 3, &s));

    // Manchester frames must not be misread as NEC.
    ir::Signal rc5;
    rc5.proto   = ir::Protocol::kRc5;
    rc5.address = 1;
    rc5.command = 2;
    ir::Timings t5;
    CHECK(ir::encode(rc5, &t5));
    ir::Signal out;
    ir::decode(t5.us, t5.count, &out);
    CHECK(out.proto != ir::Protocol::kNec);
    CHECK(out.proto != ir::Protocol::kSamsung32);
}

void testNames()
{
    CHECK(std::strcmp(ir::protocolName(ir::Protocol::kNec), "NEC") == 0);
    CHECK(std::strcmp(ir::protocolName(ir::Protocol::kSamsung32), "Samsung32") == 0);
    CHECK(std::strcmp(ir::protocolName(ir::Protocol::kUnknown), "UNKNOWN") == 0);

    CHECK(ir::protocolFromName("NEC") == ir::Protocol::kNec);
    CHECK(ir::protocolFromName("necext") == ir::Protocol::kNecExt);
    CHECK(ir::protocolFromName("SAMSUNG32") == ir::Protocol::kSamsung32);
    CHECK(ir::protocolFromName("RC5X") == ir::Protocol::kRc5X);
    CHECK(ir::protocolFromName("SIRC20") == ir::Protocol::kSirc20);
    CHECK(ir::protocolFromName("Kaseikyo") == ir::Protocol::kKaseikyo);
    CHECK(ir::protocolFromName("RCA") == ir::Protocol::kUnknown);   // not synthesised
    CHECK(ir::protocolFromName("") == ir::Protocol::kUnknown);
    CHECK(ir::protocolFromName(nullptr) == ir::Protocol::kUnknown);

    CHECK(ir::protocolSupported("NEC"));
    CHECK(ir::protocolSupported("RC6"));
    CHECK(!ir::protocolSupported("RCA"));
    CHECK(!ir::protocolSupported("NOODLE"));

    CHECK_EQ(ir::carrierHz(ir::Protocol::kSirc), 40000);
    CHECK_EQ(ir::carrierHz(ir::Protocol::kRc5), 36000);
    CHECK_EQ(ir::carrierHz(ir::Protocol::kNec), 38000);
}

void testDescribe()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kNec;
    s.address = 0x04;
    s.command = 0x08;
    char buf[64];
    ir::describe(s, buf, sizeof(buf));
    CHECK(std::strcmp(buf, "NEC A=4 C=8") == 0);
}

// Round-trip every encodable protocol with an assortment of payloads, which is
// where off-by-one bit ordering shows up.
void testRoundTripAll()
{
    const ir::Protocol protos[] = {ir::Protocol::kNec,     ir::Protocol::kNecExt,
                                   ir::Protocol::kSamsung32, ir::Protocol::kRc5,
                                   ir::Protocol::kRc5X,    ir::Protocol::kRc6,
                                   ir::Protocol::kSirc,    ir::Protocol::kSirc15,
                                   ir::Protocol::kSirc20,  ir::Protocol::kKaseikyo};
    const uint32_t values[] = {0, 1, 2, 0x55, 0x7F, 0x80, 0xFF};

    for (const ir::Protocol p : protos) {
        for (const uint32_t v : values) {
            ir::Signal s;
            s.proto   = p;
            s.address = v;
            s.command = v;
            ir::Timings t;
            if (!ir::encode(s, &t)) {
                check(false, "encode failed for a protocol/value pair", __LINE__);
                continue;
            }
            if (!wellFormed(t)) {
                check(false, "encoded frame is not well formed", __LINE__);
                continue;
            }
            ir::Signal back;
            if (!ir::decode(t.us, t.count, &back)) {
                check(false, "round trip decode failed", __LINE__);
                continue;
            }
            // NEC and NECext are bit-identical when the payload happens to look
            // like complement pairs (NECext 0x00FF/0x00FF *is* NEC 0xFF/0xFF).
            // Either label is a correct decode of that waveform.
            const bool necAlias = (p == ir::Protocol::kNecExt && back.proto == ir::Protocol::kNec);
            if (back.proto != p && !necAlias) {
                check(false, "round trip changed the protocol", __LINE__);
                continue;
            }
            // Payload width per protocol: only compare the bits the protocol keeps.
            int addrBits = 8, cmdBits = 8;
            switch (p) {
                case ir::Protocol::kNecExt: addrBits = 16; cmdBits = 16; break;
                case ir::Protocol::kRc5: addrBits = 5; cmdBits = 6; break;
                case ir::Protocol::kRc5X: addrBits = 5; cmdBits = 7; break;
                case ir::Protocol::kSirc: addrBits = 5; cmdBits = 7; break;
                case ir::Protocol::kSirc15: addrBits = 8; cmdBits = 7; break;
                case ir::Protocol::kSirc20: addrBits = 13; cmdBits = 7; break;
                case ir::Protocol::kKaseikyo: addrBits = 26; cmdBits = 10; break;
                default: break;
            }
            const uint32_t am = (1u << addrBits) - 1;
            const uint32_t cm = (1u << cmdBits) - 1;
            if (back.address != (s.address & am) || back.command != (s.command & cm)) {
                ++g_failures;
                ++g_checks;
                std::printf("FAIL %d: %s round trip v=%02lX -> %lX/%lX\n", __LINE__,
                            ir::protocolName(p), static_cast<unsigned long>(v),
                            static_cast<unsigned long>(back.address),
                            static_cast<unsigned long>(back.command));
            } else {
                ++g_checks;
            }
        }
    }
}

// Timings must survive a 5 % stretch, which is what a cheap receiver at the
// edge of its tolerance produces.
void testTolerance()
{
    ir::Signal s;
    s.proto   = ir::Protocol::kNec;
    s.address = 0x04;
    s.command = 0x08;
    ir::Timings t;
    CHECK(ir::encode(s, &t));

    uint16_t stretched[ir::kMaxTimings];
    for (int i = 0; i < t.count; ++i) stretched[i] = static_cast<uint16_t>(t.us[i] * 105 / 100);

    ir::Signal back;
    CHECK(ir::decode(stretched, t.count, &back));
    CHECK_EQ(back.address, 0x04);
    CHECK_EQ(back.command, 0x08);
}

// ---------------------------------------------------------------------------
// Gree air conditioner
//
// The first vector is a real GREE YAPOF "power on" transmission, trimmed from
// Flipper-IRDB ACs/GREE/GREE_YAPOF.ir.  It is the only way to prove the encoder
// here matches what a Gree remote actually puts on the air: decode it, then
// check that re-encoding the recovered state reproduces the same waveform.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Gree air conditioner
//
// The vector is a real Gree capture (IRremoteESP8266's own unit test, ref
// upstream issue #386) whose decoded state is 19 0A 60 50 02 23 00 F0.  It is
// the only way to prove the encoder here matches what a Gree remote puts on the
// air: decode it, then check that re-encoding the recovered state reproduces
// the same waveform.  Every frame in Flipper-IRDB's ACs/GREE directory (21 of
// them, four files) also passes this checksum, so the algorithm is not a
// one-sample guess.
// ---------------------------------------------------------------------------
static const uint16_t kGreeCapture[] = {
    9008, 4496, 644, 1660, 676, 530, 648, 558, 672, 1636, 646, 1660, 644, 556, 650, 584,
    626, 560, 644, 580, 628, 1680, 624, 560, 648, 1662, 644, 582, 648, 536, 674, 530,
    646, 580, 628, 560, 670, 532, 646, 562, 644, 556, 672, 536, 648, 1662, 646, 1660,
    652, 554, 644, 558, 672, 538, 644, 560, 668, 560, 648, 1638, 668, 536, 644, 1660,
    668, 532, 648, 560, 648, 1660, 674, 554, 622, 19990, 646, 580, 624, 1660, 648, 556,
    648, 558, 674, 556, 622, 560, 644, 564, 668, 536, 646, 1662, 646, 1658, 672, 534,
    648, 558, 644, 562, 648, 1662, 644, 584, 622, 558, 648, 562, 668, 534, 670, 536,
    670, 532, 672, 536, 646, 560, 646, 558, 648, 558, 670, 534, 650, 558, 646, 560,
    646, 560, 668, 1638, 646, 1662, 646, 1660, 646, 1660, 648,
};
constexpr int kGreeCaptureCount = static_cast<int>(sizeof(kGreeCapture) / sizeof(kGreeCapture[0]));

void testGreeCapture()
{
    CHECK_EQ(kGreeCaptureCount, 139);

    ir::Signal s;
    CHECK(ir::decode(kGreeCapture, kGreeCaptureCount, &s));
    CHECK(s.proto == ir::Protocol::kGree);

    ir::gree::State st;
    CHECK(ir::gree::fromSignal(s, &st));
    CHECK(ir::gree::valid(st));

    // The reference state for this capture, from IRremoteESP8266's unit test.
    static const uint8_t kExpected[8] = {0x19, 0x0A, 0x60, 0x50, 0x02, 0x23, 0x00, 0xF0};
    for (int i = 0; i < 8; ++i) CHECK_EQ(st.raw[i], kExpected[i]);

    // The two fixed nibbles every Gree remote sends.
    CHECK_EQ(st.raw[3] & 0xF0, 0x50);
    CHECK_EQ(st.raw[5] & 0x38, 0x20);
    // This capture is the YAW1F family: the model-A bit is set in byte 2.
    CHECK_EQ(st.raw[2] & 0x40, 0x40);

    ir::Timings t;
    CHECK(ir::encode(s, &t));
    // 2 leader + 32 bits + 3 marker bits + 1 gap + 32 bits + 1 gap = 140 entries.
    CHECK_EQ(t.count, 140);

    int outside = 0;
    for (int i = 0; i < kGreeCaptureCount - 1; ++i) {
        const int want = kGreeCapture[i];
        const int got  = t.us[i];
        if (got < want * 88 / 100 || got > want * 112 / 100) {
            if (outside == 0) {
                std::printf("  gree timing %d: capture %d, encoder %d\n", i, want, got);
            }
            ++outside;
        }
    }
    CHECK_EQ(outside, 0);
    CHECK_EQ(t.frequency, 38000);
}

void testGreeState()
{
    const ir::gree::State st =
        ir::gree::makeState(true, ir::gree::kModeCool, 24, ir::gree::kFanMed, true, true);
    CHECK(ir::gree::valid(st));
    CHECK_EQ(st.raw[1], 24 - 16);
    CHECK_EQ(st.raw[3], 0x50);
    CHECK_EQ(st.raw[5], 0x20);
    CHECK_EQ(st.raw[0] & 0x07, ir::gree::kModeCool);
    CHECK((st.raw[0] & 0x08) != 0);            // power
    CHECK_EQ((st.raw[0] >> 4) & 0x03, ir::gree::kFanMed);
    CHECK((st.raw[0] & 0x40) != 0);            // swing auto

    const ir::Signal sig = ir::gree::toSignal(st);
    ir::Timings t;
    CHECK(ir::encode(sig, &t));

    ir::Signal back;
    CHECK(ir::decode(t.us, t.count, &back));
    CHECK(back.proto == ir::Protocol::kGree);
    ir::gree::State same;
    CHECK(ir::gree::fromSignal(back, &same));
    CHECK(std::memcmp(same.raw, st.raw, ir::gree::kStateBytes) == 0);

    // Temperature clamps to the range the protocol can express.
    CHECK_EQ(ir::gree::makeState(true, 1, 5, 0, false, true).raw[1], 0);
    CHECK_EQ(ir::gree::makeState(true, 1, 40, 0, false, true).raw[1], 14);
    // A state whose checksum was not recomputed must be rejected.
    ir::gree::State broken = st;
    broken.raw[7] = static_cast<uint8_t>(broken.raw[7] ^ 0x10);
    CHECK(!ir::gree::valid(broken));

    CHECK(std::strcmp(ir::gree::modeName(ir::gree::kModeHeat), "HEAT") == 0);
    CHECK(std::strcmp(ir::gree::fanName(ir::gree::kFanMax), "HIGH") == 0);
    CHECK(ir::protocolFromName("Gree") == ir::Protocol::kGree);
    CHECK(ir::protocolSupported("GREE"));
}

// The two Gree families differ in exactly two bytes, and sending the wrong one
// gets no reaction from the unit at all -- so both have to be reachable, and the
// bits have to be where the real captures put them.
void testGreeModels()
{
    const auto yaw = ir::gree::makeState(true, ir::gree::kModeCool, 24, ir::gree::kFanAuto, false,
                                         true, ir::gree::kModelYaw1F);
    const auto yap = ir::gree::makeState(true, ir::gree::kModeCool, 24, ir::gree::kFanAuto, false,
                                         true, ir::gree::kModelYapof);
    CHECK(ir::gree::valid(yaw));
    CHECK(ir::gree::valid(yap));

    // YAW1F: model-A bit while running, 0b100 in byte 5 bits 3..5.
    CHECK_EQ(yaw.raw[2] & 0x40, 0x40);
    CHECK_EQ(yaw.raw[5], 0x20);
    // YAPOF (every capture in Flipper-IRDB's GREE directory): no model-A bit,
    // WiFi bit in byte 5 instead.
    CHECK_EQ(yap.raw[2] & 0x40, 0x00);
    CHECK_EQ(yap.raw[5], 0x40);

    // The model-A bit is asserted only while the unit runs.
    const auto yawOff = ir::gree::makeState(false, ir::gree::kModeCool, 24, ir::gree::kFanAuto,
                                            false, true, ir::gree::kModelYaw1F);
    CHECK_EQ(yawOff.raw[2] & 0x40, 0x00);
    CHECK((yawOff.raw[0] & 0x08) == 0);

    // Both families survive the encoder and the decoder.
    static const uint8_t kModels[2] = {ir::gree::kModelYaw1F, ir::gree::kModelYapof};
    for (const uint8_t model : kModels) {
        const auto st = ir::gree::makeState(true, ir::gree::kModeHeat, 30, ir::gree::kFanMax, true,
                                            true, model);
        ir::Timings t;
        CHECK(ir::encode(ir::gree::toSignal(st), &t));
        ir::Signal back;
        CHECK(ir::decode(t.us, t.count, &back));
        CHECK(back.proto == ir::Protocol::kGree);
        ir::gree::State same;
        CHECK(ir::gree::fromSignal(back, &same));
        CHECK(std::memcmp(same.raw, st.raw, ir::gree::kStateBytes) == 0);
    }
}

}  // namespace

int main()
{
    testNec();
    testNecRepeatFrame();
    testNecExt();
    testSamsung();
    testSirc();
    testRc5();
    testRc6();
    testKaseikyo();
    testGreeCapture();
    testGreeState();
    testGreeModels();
    testRejects();
    testNames();
    testDescribe();
    testRoundTripAll();
    testTolerance();

    if (g_failures == 0) {
        std::printf("ir protocol: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("ir protocol: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
