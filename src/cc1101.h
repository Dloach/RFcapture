/*
 * Minimal CC1101 driver for the CAP.CC1101 module.
 *
 * The chip is used in asynchronous OOK mode: in RX the demodulated envelope is
 * output on GDO0 (G15) and sampled by a GPIO interrupt in OokReceiver, in TX the
 * baseband pulses are fed into GDO0 by the host.  This keeps one code path for capture
 * and replay and works for every OOK remote regardless of its encoding.
 */

#pragma once

#include <Arduino.h>

#include "rf_config.h"

class Cc1101 {
public:
    bool begin();

    bool ready() const { return ready_; }
    uint8_t partnum() const { return partnum_; }
    uint8_t version() const { return version_; }

    /// Retune the chip and the antenna switch for one of the three bands.
    bool setBand(RfBand band);
    RfBand band() const { return band_; }

    /// Tune to an arbitrary frequency.  The antenna switch and the full register
    /// set are only re-applied when the band actually changes; within a band a
    /// plain retune is enough.  Used by the RSSI scanner.
    bool tuneTo(RfBand band, uint32_t hz);

    /// Write FREQ2/FREQ1/FREQ0 for a frequency in Hz (26 MHz crystal).
    void setFrequencyHz(uint32_t hz);

    /// Asynchronous OOK receive: GDO0 becomes the demodulated data output.
    bool enterRx();

    /// Asynchronous OOK transmit: GDO0 becomes the data input.
    bool enterTx();

    /// Diagnostic helpers.
    uint8_t marcState() { return readReg(0x35) & 0x1F; }
    uint8_t iocfg0() { return readReg(0x02); }

    /// Signal strength of whatever is on air right now, in dBm (CC1101
    /// datasheet conversion of the RSSI status register).  Only meaningful
    /// while the chip receives; around -100 dBm means "nothing there".
    int rssiDbm();

    /// Bring-up helper: force IOCFG0 (GDO0) to a static function so the wiring
    /// can be checked with a digitalRead.
    void debugSetIocfg0(uint8_t value) { writeReg(0x02, value); }

private:
    void setPaTable(uint8_t offLevel, uint8_t onLevel);

    uint8_t readReg(uint8_t addr);
    void writeReg(uint8_t addr, uint8_t value);
    void strobe(uint8_t command);

    bool ready_    = false;
    uint8_t partnum_ = 0;
    uint8_t version_ = 0;
    RfBand band_     = RfBand::k315;
};

extern Cc1101 g_cc1101;
