/*
 * Host side unit test for src/ook_decoder.cpp.
 *
 * It synthesises EV1527 / PT2262 pulse trains the way a 433 MHz receiver plus
 * the RMT capture would produce them (including jitter, the long sync pulse and
 * the trailing idle level) and checks that the decoder recovers the code.
 *
 * Build & run from the project root:
 *   tests/host/run.ps1
 */

#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstring>

#include "ook_decoder.h"
#include "capture_store.h"
#include "quick_bind.h"
#include "saved_store.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        printf("  [ok]   %s\n", what);
        g_pass++;
    } else {
        printf("  [FAIL] %s\n", what);
        g_fail++;
    }
}

struct Builder {
    RfFrame f;

    void add(uint32_t us, uint8_t level)
    {
        if (f.count >= rfcfg::maxFramePulses) return;
        if (us < 1) us = 1;
        if (us > 0xFFFF) us = 0xFFFF;
        f.pulses[f.count].us    = static_cast<uint16_t>(us);
        f.pulses[f.count].level = level;
        f.count++;
    }

    void extendLastLow(uint32_t extra)
    {
        if (f.count == 0) return;
        RfPulse& p = f.pulses[f.count - 1];
        uint32_t v = p.us + extra;
        p.us       = static_cast<uint16_t>(v > 0xFFFF ? 0xFFFF : v);
    }
};

// Small deterministic jitter, +/- percent.
int32_t jitter(int32_t us, int percent)
{
    static uint32_t seed = 12345;
    seed                 = seed * 1103515245u + 12345u;
    const int32_t span   = (us * percent) / 100;
    if (span == 0) return us;
    const int32_t delta = int32_t((seed >> 16) % uint32_t(2 * span + 1)) - span;
    return us + delta;
}

Builder makeEv1527(uint32_t code, int bits, int t_us, int repeats, int jitPercent)
{
    Builder b;
    for (int r = 0; r < repeats; ++r) {
        b.add(jitter(t_us, jitPercent), 1);          // sync high
        b.add(jitter(31 * t_us, jitPercent), 0);     // sync low
        for (int i = bits - 1; i >= 0; --i) {
            const bool one = ((code >> i) & 1u) != 0;
            if (one) {
                b.add(jitter(3 * t_us, jitPercent), 1);
                b.add(jitter(1 * t_us, jitPercent), 0);
            } else {
                b.add(jitter(1 * t_us, jitPercent), 1);
                b.add(jitter(3 * t_us, jitPercent), 0);
            }
        }
        // Silence between repeats merges into the last low level.
        b.extendLastLow(r + 1 == repeats ? 24000 : 11000);
    }
    return b;
}

Builder makePt2262(const int symbols[12], int t_us, int jitPercent)
{
    Builder b;
    b.add(t_us, 1);
    b.add(31 * t_us, 0);
    for (int i = 0; i < 12; ++i) {
        switch (symbols[i]) {
            case 0:  // 1T high, 3T low
                b.add(jitter(t_us, jitPercent), 1);
                b.add(jitter(3 * t_us, jitPercent), 0);
                break;
            case 1:  // 3T high, 1T low
                b.add(jitter(3 * t_us, jitPercent), 1);
                b.add(jitter(t_us, jitPercent), 0);
                break;
            default:  // floating: 1T / 1T
                b.add(jitter(t_us, jitPercent), 1);
                b.add(jitter(t_us, jitPercent), 0);
                break;
        }
    }
    b.extendLastLow(24000);
    return b;
}

void testEv1527()
{
    printf("EV1527 24 bit frames\n");

    const uint32_t codes[] = {0xA1B2C3ul, 0x000000ul, 0xFFFFFFul, 0x0F1E2Dul};
    for (uint32_t code : codes) {
        const Builder b = makeEv1527(code, 24, 340, 3, 6);
        const Decoded d = decodeFrame(b.f);
        char what[96];
        snprintf(what, sizeof(what), "code %06lX -> valid/ev1527/bits24/code match (got %s %06lX bits %u)",
                 static_cast<unsigned long>(code), protocolName(d.proto),
                 static_cast<unsigned long>(d.code & 0xFFFFFFul), static_cast<unsigned>(d.bits));
        check(d.valid && d.proto == RfProtocol::kEv1527 && d.bits == 24 &&
                  (d.code & 0xFFFFFFul) == code,
              what);
    }

    // Different base times as found on real remotes.
    for (int t : {250, 300, 380, 450}) {
        const Builder b = makeEv1527(0x123456ul, 24, t, 2, 4);
        const Decoded d = decodeFrame(b.f);
        char what[96];
        snprintf(what, sizeof(what), "T=%dus -> base %uus within 10%%", t, static_cast<unsigned>(d.baseUs));
        check(d.valid && std::abs(int(d.baseUs) - t) <= t / 10, what);
    }
}

void testPt2262()
{
    printf("PT2262 12 symbol frames\n");

    const int symbols[12] = {0, 1, 2, 0, 0, 1, 1, 2, 0, 1, 2, 1};
    const Builder b      = makePt2262(symbols, 320, 5);
    const Decoded d      = decodeFrame(b.f);

    uint32_t expected = 0;
    for (int i = 0; i < 12; ++i) expected = (expected << 1) | (symbols[i] == 1 ? 1u : 0u);

    char what[96];
    snprintf(what, sizeof(what), "12 symbols -> pt2262, code %03lX, 3 floating (got %s %03lX tri %u)",
             static_cast<unsigned long>(expected), protocolName(d.proto),
             static_cast<unsigned long>(d.code & 0xFFFul), static_cast<unsigned>(d.triStates));
    check(d.valid && d.proto == RfProtocol::kPt2262 && d.bits == 12 && d.triStates == 3 &&
              (d.code & 0xFFFul) == expected,
          what);
}

void testPartialFrame()
{
    printf("Partial captures\n");

    // Capture starts after the sync pulse: data bits only.
    Builder b = makeEv1527(0x0A0B0Cul, 24, 330, 1, 3);
    RfFrame cut;
    for (int i = 2; i < b.f.count && cut.count < rfcfg::maxFramePulses; ++i) {
        cut.pulses[cut.count++] = b.f.pulses[i];
    }
    const Decoded d = decodeFrame(cut);
    check(d.valid && (d.code & 0xFFFFFFul) == 0x0A0B0Cul,
          "frame without sync pulse still decodes (fallback path)");

    // Garbage must not decode.
    Builder g;
    for (int i = 0; i < 40; ++i) {
        g.add(static_cast<uint32_t>(100 + (i * 37) % 900), (i % 2) ? 0 : 1);
    }
    const Decoded bad = decodeFrame(g.f);
    check(!bad.valid, "random pulse train is rejected");
}

void testBitsFormatting()
{
    Decoded d;
    d.bits = 8;
    d.code = 0xA5;
    char out[16];
    formatBits(d, out, sizeof(out));
    check(std::strcmp(out, "10100101") == 0, "formatBits renders MSB first");
}

// Copy the widths out of a Builder the way CaptureStore does: levels are thrown
// away and implied by index parity.
int widthsOf(const Builder& b, uint16_t* out, int max)
{
    const int n = b.f.count < max ? b.f.count : max;
    for (int i = 0; i < n; ++i) out[i] = b.f.pulses[i].us;
    return n;
}

void testEstimateBaseUs()
{
    printf("Base time estimation\n");

    for (int t : {250, 340, 450}) {
        const Builder b    = makeEv1527(0x123456ul, 24, t, 1, 6);
        const uint16_t est = estimateBaseUs(b.f);
        char what[96];
        snprintf(what, sizeof(what), "T=%dus -> estimate %uus within 10%%", t,
                 static_cast<unsigned>(est));
        check(est != 0 && std::abs(int(est) - t) <= t / 10, what);
    }

    // A glitch wider than the noise floor but far below 1T: the plain minimum
    // would return 90 and every 1T/3T window would then miss.
    Builder noisy = makeEv1527(0x123456ul, 24, 340, 1, 4);
    RfFrame withSpike;
    withSpike.pulses[withSpike.count].us    = 90;
    withSpike.pulses[withSpike.count].level = 1;
    withSpike.count++;
    for (int i = 0; i < noisy.f.count && withSpike.count < rfcfg::maxFramePulses; ++i) {
        withSpike.pulses[withSpike.count++] = noisy.f.pulses[i];
    }

    uint16_t plainMin = 0xFFFF;
    for (int i = 0; i < withSpike.count; ++i) {
        if (withSpike.pulses[i].us < plainMin) plainMin = withSpike.pulses[i].us;
    }
    const uint16_t est = estimateBaseUs(withSpike);
    char what[128];
    snprintf(what, sizeof(what),
             "90us glitch ignored (plain min %uus, estimate %uus ~ 340us)",
             static_cast<unsigned>(plainMin), static_cast<unsigned>(est));
    check(plainMin == 90 && est >= 306 && est <= 374, what);

    // Uniform noise can still produce a spurious width consensus, so the
    // invariant that matters is that it never scores like a frame.
    Builder g;
    for (int i = 0; i < 40; ++i) {
        g.add(static_cast<uint32_t>(100 + (i * 37) % 900), (i % 2) ? 0 : 1);
    }
    uint16_t gw[rfcfg::maxFramePulses];
    const int gn      = widthsOf(g, gw, rfcfg::maxFramePulses);
    const uint16_t ge = estimateBaseUs(g.f);
    snprintf(what, sizeof(what), "random pulse train scores %u (< 50), estimate %uus",
             static_cast<unsigned>(scoreWaveform(gw, gn, ge > 0 ? ge : 340)),
             static_cast<unsigned>(ge));
    check(scoreWaveform(gw, gn, ge > 0 ? ge : 340) < 50, what);
}

void testScoreWaveform()
{
    printf("Waveform quality score\n");

    // One clean frame.  The trailing silence a single-repeat capture merges into
    // the last low is dropped, because CaptureStore::ingest() trims the stored
    // train to exactly one frame.
    Builder clean = makeEv1527(0x123456ul, 24, 340, 1, 6);
    if (clean.f.count > 0) clean.f.count--;
    uint16_t w[rfcfg::maxFramePulses];
    const int n         = widthsOf(clean, w, rfcfg::maxFramePulses);
    const uint8_t cleanScore = scoreWaveform(w, n, 340);

    char what[96];
    snprintf(what, sizeof(what), "clean EV1527 frame scores %u (>= 85)",
             static_cast<unsigned>(cleanScore));
    check(cleanScore >= 85, what);

    Builder g;
    for (int i = 0; i < 40; ++i) {
        g.add(static_cast<uint32_t>(100 + (i * 37) % 900), (i % 2) ? 0 : 1);
    }
    uint16_t gw[rfcfg::maxFramePulses];
    const int gn         = widthsOf(g, gw, rfcfg::maxFramePulses);
    const uint8_t badScore = scoreWaveform(gw, gn, 340);
    snprintf(what, sizeof(what), "noise scores %u, well below the clean frame's %u",
             static_cast<unsigned>(badScore), static_cast<unsigned>(cleanScore));
    check(badScore + 20 < cleanScore, what);

    // A frame whose sync is stretched: shape is still perfect, so only the sync
    // term may pull the score down.
    Builder stretch = makeEv1527(0x123456ul, 24, 340, 1, 2);
    if (stretch.f.count > 0) stretch.f.count--;
    stretch.f.pulses[1].us = 31 * 340 * 3 / 2;  // 46T instead of 31T
    uint16_t sw[rfcfg::maxFramePulses];
    const int sn            = widthsOf(stretch, sw, rfcfg::maxFramePulses);
    const uint8_t syncScore = scoreWaveform(sw, sn, 340);
    snprintf(what, sizeof(what), "46T sync scores %u, below the clean frame's %u",
             static_cast<unsigned>(syncScore), static_cast<unsigned>(cleanScore));
    check(syncScore < cleanScore, what);

    check(scoreWaveform(nullptr, 10, 340) == 0, "null train scores 0");
    check(scoreWaveform(w, n, 0) == 0, "missing base time scores 0");
    check(scoreWaveform(w, 4, 340) == 0, "train too short scores 0");

    // Two back-to-back frames: every sync sits on the same index parity.  The
    // trailing 24 ms of idle silence is dropped for the same reason as above.
    Builder two = makeEv1527(0x123456ul, 24, 340, 2, 3);
    if (two.f.count > 0) two.f.count--;
    uint16_t ok[rfcfg::maxFramePulses];
    const int nOk         = widthsOf(two, ok, rfcfg::maxFramePulses);
    const uint8_t twoScore = scoreWaveform(ok, nOk, 340);
    snprintf(what, sizeof(what), "two clean frames score %u (>= 85)",
             static_cast<unsigned>(twoScore));
    check(twoScore >= 85, what);

    // Splice one extra pulse in after the first sync: everything past it shifts
    // by one, so the later syncs land on the opposite parity.  This is what a
    // single missed edge in the receiver ISR does to a stored train, and the
    // shape and sync terms both stay near perfect, so only the parity test sees
    // it.  This is the signature a single missed edge leaves on a stored train;
    // it was first seen on real hardware, where a capture that decoded to the
    // same code as a known-good one still would not open the gate.
    uint16_t broken[rfcfg::maxFramePulses];
    int nBad = 0;
    for (int i = 0; i < nOk && nBad < rfcfg::maxFramePulses; ++i) {
        if (i == 6) broken[nBad++] = 340;
        broken[nBad++] = ok[i];
    }
    const uint8_t badParity = scoreWaveform(broken, nBad, 340);
    snprintf(what, sizeof(what), "syncs on both parities score %u (capped below 40)",
             static_cast<unsigned>(badParity));
    check(badParity <= 39, what);
}

void testBindingKeys()
{
    printf("Quick-bind stable keys\n");

    const uint16_t a[4] = {340, 1020, 340, 1020};
    const uint16_t b[4] = {338, 1018, 342, 1022};
    const uint16_t c[4] = {1000, 340, 1000, 340};

    check(quickbind::keyForRf(0, true, 0x123456u, 24, a, 4) ==
              quickbind::keyForRf(0, true, 0x123456u, 24, b, 4),
          "decoded RF key ignores waveform jitter");
    check(quickbind::keyForRf(0, true, 0x123456u, 24, a, 4) !=
              quickbind::keyForRf(1, true, 0x123456u, 24, a, 4),
          "decoded RF key includes the band");
    check(quickbind::keyForRf(0, false, 0, 0, a, 4) !=
              quickbind::keyForRf(0, false, 0, 0, c, 4),
          "RAW RF key changes with the pulse train");
    check(quickbind::keyForRf(0, true, 0, 24, a, 4) != 0 &&
              quickbind::keyForRf(0, true, 0, 24, a, 4) !=
                  quickbind::keyForRf(0, false, 0, 0, a, 4),
          "decoded code 0 is legal and stays distinct from RAW");
    check(quickbind::keyForRf(0, true, 0x123456u, 24, a, 4) !=
              quickbind::keyForRf(0, true, 0x123456u, 12, a, 4),
          "decoded RF key includes the bit count");

    const uint8_t uid1[4] = {0x3C, 0x02, 0xB6, 0x11};
    const uint8_t uid2[4] = {0x3C, 0x02, 0xB6, 0x12};
    check(quickbind::keyForNfc(uid1, 4) == quickbind::keyForNfc(uid1, 4) &&
              quickbind::keyForNfc(uid1, 4) != quickbind::keyForNfc(uid2, 4) &&
              quickbind::keyForNfc(uid1, 4) != 0,
          "NFC key follows the UID");
}

void testWaveformReplacementPolicy()
{
    printf("Waveform replacement policy\n");
    check(shouldReplaceWaveform(0, 1), "empty score can be replaced");
    check(shouldReplaceWaveform(30, 60), "bad waveform heals at q60");
    check(shouldReplaceWaveform(50, 60), "ten point gain replaces");
    check(!shouldReplaceWaveform(50, 55), "five point gain does not rewrite NVS");
    check(!shouldReplaceWaveform(90, 95), "small gain on a good waveform is ignored");
}

void testSavedRecoveryCount()
{
    printf("Permanent-list torn-write recovery\n");
    const size_t rec = sizeof(SavedSignal);
    check(recoverSavedCount(rec * 3, 3, rec, 8) == 3, "complete blob + matching count");
    check(recoverSavedCount(rec * 3, 2, rec, 8) == 2, "uncommitted tail is ignored");
    check(recoverSavedCount(rec * 3, 5, rec, 8) == 3, "count larger than blob is clamped");
    check(recoverSavedCount(rec * 3 + 1, 3, rec, 8) == 3, "partial tail keeps committed records");
    check(recoverSavedCount(rec * 2 + 1, 3, rec, 8) == 2, "short blob recovers its whole records");
    check(recoverSavedCount(0, 0, rec, 8) == 0, "empty store restores zero");
    check(recoverSavedCount(rec * 10, 10, rec, 8) == 8, "blob is clamped to the array size");
}

}  // namespace

int main()
{
    printf("=== 433 decoder host tests ===\n");
    testEv1527();
    testPt2262();
    testPartialFrame();
    testBitsFormatting();
    testEstimateBaseUs();
    testScoreWaveform();
    testBindingKeys();
    testWaveformReplacementPolicy();
    testSavedRecoveryCount();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
