#include "ook_decoder.h"

namespace {

// A train whose level labels were inverted mid-burst replays the complement of
// what was on air, so it has to land in the same band as noise rather than
// merely "a bit lower" - the score is read off the list as plain text.
constexpr int kScoreUnusableCap = 39;

struct PairResult {
    int count        = 0;   // decoded pulse pairs (bits)
    int complete     = 0;   // pairs whose low half was not cut off by the gap
    uint32_t code    = 0;
    int tri          = 0;
    uint32_t totalUs = 0;
};

// Classify one pulse pair by its high/low ratio, which is independent of the
// base time: 1T/3T and 3T/1T both sum to 4T, and a floating PT2262 symbol to 2T.
// Returns 0 for a logical 0, 1 for a logical 1, 2 for a floating symbol.
int classifyPair(float hi, float lo)
{
    const float total = hi + lo;
    if (total <= 0.0f) return 0;
    const float ratio = hi / total;
    if (ratio <= 0.36f) return 0;
    if (ratio >= 0.64f) return 1;
    return 2;
}

PairResult parsePairs(const RfFrame& frame, int idx, float t)
{
    PairResult r;
    const int n = frame.count;
    if (t <= 0.0f) return r;

    // Exactly one EV1527 frame is 24 bits; a captured burst usually contains
    // several back-to-back frames, and parsing past the first 24 symbols would
    // fold the following frames (and their sync gap) into the code, making the
    // decoded value drift from capture to capture.
    for (int i = 0; i < 24 && idx < n; ++i) {
        if (frame.pulses[idx].level != 1) break;

        const float hi = frame.pulses[idx].us;
        if (hi < 0.5f * t) break;  // far too short, not part of a valid bit

        if (idx + 1 >= n) {
            // The burst ended on a high pulse - classify on the high width only.
            r.code = (r.code << 1) | (hi > 2.0f * t ? 1u : 0u);
            r.count++;
            break;
        }

        const float lo = frame.pulses[idx + 1].us;
        if (frame.pulses[idx + 1].level != 0) break;

        const float total = hi + lo;

        if (total > 6.5f * t) {
            // Last bit of a frame: the silence that follows the burst is merged
            // into this low level, so only the high pulse can be measured.
            r.code = (r.code << 1) | (hi > 2.0f * t ? 1u : 0u);
            r.count++;
            break;
        }
        if (total < 1.4f * t) break;  // neither 4T nor a floating 2T pair

        const int sym = classifyPair(hi, lo);

        r.code = (r.code << 1) | uint32_t(sym == 1 ? 1 : 0);
        r.count++;
        if (sym == 2) r.tri++;

        // Only full 1T/3T pairs carry a clean 4T period; use them to refine T.
        if (total > 3.0f * t && total < 5.0f * t) {
            r.totalUs += uint32_t(total);
            r.complete++;
        }

        idx += 2;
    }
    return r;
}

Decoded buildResult(const PairResult& pr, float t)
{
    Decoded d;
    d.bits      = uint8_t(pr.count);
    d.code      = pr.code;
    d.triStates = uint8_t(pr.tri);

    float refined = t;
    if (pr.complete >= 4 && pr.totalUs > 0) {
        refined = float(pr.totalUs) / (4.0f * float(pr.complete));
    }
    if (refined < 60.0f) refined = t;
    d.baseUs = uint16_t(refined + 0.5f);

    if (pr.tri > 0 || pr.count == 12) {
        d.proto = RfProtocol::kPt2262;
    } else if (pr.count >= 22) {
        d.proto = RfProtocol::kEv1527;
        // EV1527 is exactly 24 bits.  A capture that started one pair early or
        // late still carries the same code in its low 24 bits, so normalise it
        // and keep the hit counter stable.  Shorter parses are noise: with 20 or
        // 21 "bits" the timing of a random noise burst fits this shape far too
        // easily and produced phantom captures.
        d.code = pr.code & 0xFFFFFFu;
        d.bits = 24;
    } else {
        d.proto = RfProtocol::kUnknown;
    }
    d.valid = (d.proto == RfProtocol::kEv1527) || (d.proto == RfProtocol::kPt2262);
    return d;
}

// Shared body of the two estimateBaseUs() overloads; `width` reads the pulse
// width at index i from whichever container the caller has.
template <typename Width>
uint16_t estimateBaseImpl(int n, Width width)
{
    uint16_t best       = 0;
    int bestSupport     = 0;
    for (int i = 0; i < n; ++i) {
        const uint32_t w = width(i);
        if (w < 40 || w > 4000) continue;

        int support = 0;
        for (int j = 0; j < n; ++j) {
            const uint32_t v = width(j);
            if (v * 5 >= w * 4 && v * 5 <= w * 6) support++;  // within +/-20 %
        }
        // A real EV1527 frame puts 24 pulses on 1T, so four supporters is a very
        // loose bar; noise widths have none.
        if (support < 4) continue;
        if (support > bestSupport || (support == bestSupport && w < best)) {
            bestSupport = support;
            best        = static_cast<uint16_t>(w);
        }
    }
    return best;
}

}  // namespace

Decoded decodeFrame(const RfFrame& frame)
{
    Decoded out;
    const int n = frame.count;
    if (n < 8) return out;

    int first = 0;
    while (first < n - 2 && frame.pulses[first].level == 0) first++;

    // The shortest pulse of the burst is one base time T.
    uint16_t minUs = 0xFFFF;
    for (int i = first; i < n; ++i) {
        if (frame.pulses[i].us < minUs) minUs = frame.pulses[i].us;
    }
    if (minUs == 0xFFFF || minUs < 40) minUs = 40;

    Decoded best;
    int bestBits = 0;

    // Look for the sync pulse: 31T of low level shortly before the data bits.
    //
    // Candidates are tried longest first.  Inside one frame the sync low is 31T
    // while every data low is at most 3T, so the longest low is unambiguous -
    // but a data low stretched by a noise glitch can also pass the 6T gate, and
    // taking that one starts the parse one pair too early, which shifts the whole
    // decoded word right by one bit and fills the top bit (0x123456 would read
    // back as 0x891A2B).  Trying the longest low first, and keeping the first
    // full-length parse, fixes that.
    int cand[32];
    int ncand = 0;
    for (int s = first; s < n - 2 && ncand < 32; ++s) {
        if (frame.pulses[s].level != 0) continue;
        const uint32_t lowUs = frame.pulses[s].us;
        if (lowUs < 6u * minUs) continue;

        const float t = float(lowUs) / 31.0f;
        if (t < 60.0f || t > 2000.0f) continue;
        cand[ncand++] = s;
    }
    // Insertion sort, descending by low duration.
    for (int i = 1; i < ncand; ++i) {
        const int key = cand[i];
        int j         = i - 1;
        while (j >= 0 && frame.pulses[cand[j]].us < frame.pulses[key].us) {
            cand[j + 1] = cand[j];
            --j;
        }
        cand[j + 1] = key;
    }

    for (int k = 0; k < ncand; ++k) {
        const int s    = cand[k];
        const float t  = float(frame.pulses[s].us) / 31.0f;
        const PairResult pr = parsePairs(frame, s + 1, t);
        if (pr.count < 12 || pr.count <= bestBits) continue;

        best     = buildResult(pr, t);
        bestBits = pr.count;
        if (bestBits >= 24) break;
    }

    // The capture may have started in the middle of a frame; try to decode it
    // directly without a sync pulse.  A full length code is required so that a
    // truncated burst does not produce a bogus entry.
    if (bestBits < 22) {
        const PairResult pr = parsePairs(frame, first, float(minUs));
        if (pr.count >= 22 && pr.count > bestBits) {
            best     = buildResult(pr, float(minUs));
            bestBits = pr.count;
        }
    }

    if (bestBits < 12) return out;
    return best;
}

const char* protocolName(RfProtocol proto)
{
    switch (proto) {
        case RfProtocol::kEv1527: return "EV1527";
        case RfProtocol::kPt2262: return "PT2262";
        default: return "RAW";
    }
}

uint16_t estimateBaseUs(const uint16_t* pulses, int count)
{
    if (pulses == nullptr) return 0;
    return estimateBaseImpl(count, [pulses](int i) -> uint32_t { return pulses[i]; });
}

uint16_t estimateBaseUs(const RfFrame& frame)
{
    return estimateBaseImpl(frame.count,
                            [&frame](int i) -> uint32_t { return frame.pulses[i].us; });
}

uint8_t scoreWaveform(const uint16_t* pulses, int count, uint16_t baseUs)
{
    if (pulses == nullptr || count < 8 || baseUs < 40) return 0;
    const uint32_t t = baseUs;

    // Same 1T / 3T windows as looksLikeOok(): the two must not disagree about
    // what counts as a clean pulse, or a capture would be accepted and then
    // shown with a low score.
    int total = 0;
    int good  = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t u = pulses[i];
        if (u >= 6u * t) continue;  // sync low, off-scale by design
        ++total;
        if (u * 10 >= t * 6 && u * 100 <= t * 145) {
            ++good;  // ~1T
        } else if (u * 10 >= t * 24 && u * 100 <= t * 360) {
            ++good;  // ~3T
        }
    }

    uint32_t longestLow = 0;
    int longCount = 0, longOdd = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t u = pulses[i];
        // A sync is 31T; only accept a window around it so that a data pulse
        // stretched by a glitch is not mistaken for one.
        if (u >= 20u * t && u <= 45u * t) {
            longCount++;
            if (i & 1) longOdd++;
        }
        if ((i & 1) && u > longestLow) longestLow = u;
    }

    int syncScore = 0;
    const uint32_t ideal = 31u * t;
    if (longestLow > 0) {
        const uint32_t err = longestLow > ideal ? longestLow - ideal : ideal - longestLow;
        const uint32_t pct = err * 100u / ideal;
        // A sync that is off by half the ideal length carries no information.
        syncScore = pct >= 50u ? 0 : static_cast<int>(100u - pct * 2u);
    }

    const int shape = total > 0 ? good * 100 / total : 0;
    int score       = (shape * 60 + syncScore * 40) / 100;

    // CaptureStore keeps widths only, so the levels are implied by index parity:
    // pulses[0] is a high, and every sync low therefore sits at an odd index.
    // When the long pulses split across both parities the labels were inverted
    // part way through the burst - one edge was missed in the receiver ISR - and
    // the transmitter replays the complement of what was actually on air.  Shape
    // and sync both stay near perfect in that case, so it needs its own test.
    if (longCount >= 2 && longOdd != 0 && longOdd != longCount) {
        if (score > kScoreUnusableCap) score = kScoreUnusableCap;
    }
    return static_cast<uint8_t>(score);
}

void formatBits(const Decoded& decoded, char* out, size_t outSize)
{
    if (outSize == 0) return;
    size_t k = 0;
    for (int i = int(decoded.bits) - 1; i >= 0 && k + 1 < outSize; --i) {
        out[k++] = ((decoded.code >> i) & 1u) ? '1' : '0';
    }
    out[k] = '\0';
}
