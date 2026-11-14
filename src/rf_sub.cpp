#include "rf_sub.h"

#include <SD.h>

#include <cstdlib>
#include <cstring>

#include "ook_decoder.h"

namespace rfsub {
namespace {

void addPulse(Signal& s, uint16_t us)
{
    if (s.count >= rfcfg::storedPulses) return;
    if (us == 0) us = 1;
    s.pulses[s.count++] = us;
}

bool startsWith(const String& s, const char* p)
{
    return s.startsWith(p);
}

String valueAfter(const String& line, const char* key)
{
    String v = line.substring(strlen(key));
    v.trim();
    return v;
}

uint32_t parseKeyBytes(const String& value, uint8_t bits)
{
    uint32_t code = 0;
    int pos = 0;
    while (pos < (int)value.length()) {
        while (pos < (int)value.length() && value[pos] == ' ') ++pos;
        if (pos + 1 >= (int)value.length()) break;
        char hex[3] = {value[pos], value[pos + 1], 0};
        code = (code << 8) | static_cast<uint32_t>(strtoul(hex, nullptr, 16));
        pos += 2;
    }
    if (bits < 32) code &= (1u << bits) - 1u;
    return code;
}

}  // namespace

uint32_t bandHz(RfBand band)
{
    switch (band) {
        case RfBand::k315: return 315000000u;
        case RfBand::k433: return 433920000u;
        case RfBand::k868: return 868350000u;
    }
    return 433920000u;
}

RfBand bandForHz(uint32_t hz)
{
    if (hz < 390000000u) return RfBand::k315;
    if (hz < 700000000u) return RfBand::k433;
    return RfBand::k868;
}

bool buildEv1527(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out)
{
    if (bits == 0 || bits > 24 || baseUs < 40) return false;
    out = Signal{};
    out.proto  = RfProtocol::kEv1527;
    out.bits   = bits;
    out.code   = code & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u));
    out.baseUs = baseUs;
    out.band   = RfBand::k433;
    addPulse(out, baseUs);       // sync high 1T
    addPulse(out, baseUs * 31);  // sync low 31T
    for (int i = bits - 1; i >= 0; --i) {
        const bool one = (out.code >> i) & 1u;
        addPulse(out, one ? baseUs * 3 : baseUs);
        addPulse(out, one ? baseUs : baseUs * 3);
    }
    return out.count > 2;
}

bool buildPt2262(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out)
{
    if (bits == 0 || bits > 12 || baseUs < 40) return false;
    out = Signal{};
    out.proto  = RfProtocol::kPt2262;
    out.bits   = bits;
    out.code   = code & ((1u << bits) - 1u);
    out.baseUs = baseUs;
    out.band   = RfBand::k433;
    addPulse(out, baseUs);
    addPulse(out, baseUs * 31);
    for (int i = bits - 1; i >= 0; --i) {
        const bool one = (out.code >> i) & 1u;
        addPulse(out, one ? baseUs * 3 : baseUs);
        addPulse(out, one ? baseUs : baseUs * 3);
    }
    return out.count > 2;
}

bool buildCame(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out)
{
    if (bits == 0 || bits > 12 || baseUs < 80) return false;
    out = Signal{};
    out.proto  = RfProtocol::kCame;
    out.bits   = bits;
    out.code   = code & ((1u << bits) - 1u);
    out.baseUs = baseUs;
    out.band   = RfBand::k433;
    addPulse(out, baseUs);         // pilot high
    addPulse(out, baseUs * 36);    // pilot low
    for (int i = bits - 1; i >= 0; --i) {
        const bool one = (out.code >> i) & 1u;
        addPulse(out, one ? baseUs : baseUs * 2);
        addPulse(out, one ? baseUs * 2 : baseUs);
    }
    addPulse(out, baseUs);
    addPulse(out, baseUs * 9);     // stop
    return out.count > 2;
}

bool buildNice(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out)
{
    if (bits == 0 || bits > 12 || baseUs < 100) return false;
    out = Signal{};
    out.proto  = RfProtocol::kNice;
    out.bits   = bits;
    out.code   = code & ((1u << bits) - 1u);
    out.baseUs = baseUs;
    out.band   = RfBand::k433;
    addPulse(out, baseUs);
    addPulse(out, baseUs * 36);
    for (int i = bits - 1; i >= 0; --i) {
        const bool one = (out.code >> i) & 1u;
        addPulse(out, one ? baseUs * 3 : baseUs);
        addPulse(out, one ? baseUs : baseUs * 3);
    }
    return out.count > 2;
}

RfProtocol decodeBrand(const RfFrame& frame, uint32_t& code, uint8_t& bits, uint16_t& baseUs)
{
    code = 0;
    bits = 0;
    baseUs = estimateBaseUs(frame);
    if (baseUs < 100 || frame.count < 12) return RfProtocol::kUnknown;

    int pilot = -1;
    for (int i = 1; i < frame.count; i += 2) {
        if (frame.pulses[i].us >= 20u * baseUs) {
            pilot = i;
            break;
        }
    }
    if (pilot < 0) return RfProtocol::kUnknown;

    int idx = pilot + 1;
    for (int b = 0; b < 12 && idx + 1 < frame.count; ++b, idx += 2) {
        if (frame.pulses[idx].level != 1 || frame.pulses[idx + 1].level != 0) break;
        const uint32_t hi = frame.pulses[idx].us;
        const uint32_t lo = frame.pulses[idx + 1].us;
        code = (code << 1) | (hi > lo ? 1u : 0u);
        ++bits;
    }
    if (bits != 12) return RfProtocol::kUnknown;
    return baseUs >= 500 ? RfProtocol::kNice : RfProtocol::kCame;
}

bool parseFile(const char* path, Signal& out)
{
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    uint32_t freq = 433920000u;
    String protocol;
    uint8_t bits = 0;
    uint16_t te = 0;
    uint32_t key = 0;
    bool haveKey = false;
    Signal raw;
    raw.band = RfBand::k433;
    int lastSign = 0;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        if (startsWith(line, "Frequency:")) {
            freq = strtoul(valueAfter(line, "Frequency:").c_str(), nullptr, 10);
        } else if (startsWith(line, "Protocol:")) {
            protocol = valueAfter(line, "Protocol:");
        } else if (startsWith(line, "Bit:")) {
            bits = static_cast<uint8_t>(valueAfter(line, "Bit:").toInt());
        } else if (startsWith(line, "TE:")) {
            te = static_cast<uint16_t>(valueAfter(line, "TE:").toInt());
        } else if (startsWith(line, "Key:")) {
            key = parseKeyBytes(valueAfter(line, "Key:"), bits ? bits : 24);
            haveKey = true;
        } else if (startsWith(line, "RAW_Data:")) {
            String data = valueAfter(line, "RAW_Data:");
            int pos = 0;
            while (pos < (int)data.length() && raw.count < rfcfg::storedPulses) {
                while (pos < (int)data.length() && data[pos] == ' ') ++pos;
                int start = pos;
                while (pos < (int)data.length() && data[pos] != ' ') ++pos;
                if (pos == start) break;
                const long v = strtol(data.substring(start, pos).c_str(), nullptr, 10);
                if (v == 0) continue;
                const int sign = v > 0 ? 1 : -1;
                const uint16_t us = static_cast<uint16_t>(labs(v) > 65000 ? 65000 : labs(v));
                if (raw.count == 0) {
                    if (sign > 0) {
                        addPulse(raw, us);
                        lastSign = 1;
                    } else {
                        addPulse(raw, 1);  // ensure the train starts high
                        addPulse(raw, us);
                        lastSign = -1;
                    }
                } else if (sign != lastSign) {
                    addPulse(raw, us);
                } else if (raw.count > 0) {
                    uint32_t sum = raw.pulses[raw.count - 1] + us;
                    raw.pulses[raw.count - 1] = static_cast<uint16_t>(sum > 65000 ? 65000 : sum);
                }
                if (raw.count > 0 && sign != lastSign) lastSign = sign;
            }
        }
    }
    f.close();

    if (raw.count >= 8) {
        raw.band = bandForHz(freq);
        raw.baseUs = te ? te : estimateBaseUs(raw.pulses, raw.count);
        raw.code = 0;
        raw.bits = 0;
        raw.proto = RfProtocol::kUnknown;
        out = raw;
        return true;
    }

    if (!haveKey || bits == 0) return false;
    RfBand band = bandForHz(freq);
    Signal built;
    if (protocol.indexOf("CAME") >= 0) {
        if (!buildCame(key, bits, te ? te : 320, built)) return false;
    } else if (protocol.indexOf("Nice") >= 0) {
        if (!buildNice(key, bits, te ? te : 700, built)) return false;
    } else {
        if (!buildEv1527(key, bits, te ? te : 350, built)) return false;
    }
    built.band = band;
    out = built;
    return true;
}

bool exportFile(const char* path, const SavedSignal& signal)
{
    SD.mkdir("/subghz");
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    const uint32_t hz = bandHz(signal.band);
    f.printf("Filetype: Flipper SubGhz RAW File\nVersion: 1\n");
    f.printf("Frequency: %lu\n", static_cast<unsigned long>(hz));
    f.printf("Preset: FuriHalSubGhzPresetOok650Async\nProtocol: RAW\n");
    int n = signal.count;
    if (n > rfcfg::storedPulses) n = rfcfg::storedPulses;
    for (int i = 0; i < n; ++i) {
        if ((i % 16) == 0) {
            if (i) f.print('\n');
            f.print("RAW_Data:");
        }
        f.printf(" %d", (i & 1) ? -static_cast<int>(signal.pulses[i]) : static_cast<int>(signal.pulses[i]));
    }
    f.print('\n');
    f.close();
    return true;
}

void safeName(const char* in, char* out, size_t outSize)
{
    if (outSize == 0) return;
    size_t k = 0;
    for (size_t i = 0; in && in[i] && k + 1 < outSize; ++i) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_') {
            out[k++] = c;
        } else if (c == ' ') {
            out[k++] = '_';
        }
    }
    if (k == 0) {
        strcpy(out, "signal");
    } else {
        out[k] = 0;
    }
}

}  // namespace rfsub
