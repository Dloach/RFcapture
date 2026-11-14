#pragma once

#include <Arduino.h>

#include "rf_config.h"
#include "saved_store.h"

namespace rfsub {

struct Signal {
    RfBand band      = RfBand::k433;
    RfProtocol proto = RfProtocol::kUnknown;
    uint32_t code    = 0;
    uint16_t baseUs  = 0;
    uint16_t count   = 0;
    uint8_t bits     = 0;
    uint16_t pulses[rfcfg::storedPulses] = {0};
};

uint32_t bandHz(RfBand band);
RfBand bandForHz(uint32_t hz);

/// Fixed-code waveform builders.  `baseUs` is the 1T time.
bool buildEv1527(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out);
bool buildPt2262(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out);
bool buildCame(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out);
bool buildNice(uint32_t code, uint8_t bits, uint16_t baseUs, Signal& out);

/// Parse one Flipper .sub file (RAW or Princeton/CAME/Nice key).  Returns false
/// when the file is missing/unsupported or has no usable pulse train.
bool parseFile(const char* path, Signal& out);

/// Best-effort CAME/Nice 12-bit PWM classifier for a live frame.  This is a
/// supplementary analyzer, not a claim that every PT2262 frame is one of them.
RfProtocol decodeBrand(const RfFrame& frame, uint32_t& code, uint8_t& bits, uint16_t& baseUs);

/// Write one saved signal as a Flipper RAW .sub file.  Creates /subghz.
bool exportFile(const char* path, const SavedSignal& signal);

/// Sanitize a name into a short .sub filename (no slash/space).
void safeName(const char* in, char* out, size_t outSize);

}  // namespace rfsub
