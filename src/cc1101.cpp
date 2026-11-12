#include "cc1101.h"

#include <SPI.h>

Cc1101 g_cc1101;

namespace {

struct RegVal {
    uint8_t reg;
    uint8_t val;
};

// Base configuration.  These are the values the official M5Stack/RadioLib
// example ends up with at 315 MHz (2.4 kbps, 25 kHz deviation, 58 kHz RX
// filter, OOK): a generic "OOK sniffer" register set leaves the demodulator
// silent on this module.
const RegVal kConfig[] = {
    {0x01, 0x2E},  // IOCFG1
    {0x03, 0x0F},  // FIFOTHR
    {0x04, 0x12},  // SYNC1
    {0x05, 0xAD},  // SYNC0
    {0x06, 0xFF},  // PKTLEN
    {0x07, 0x64},  // PKTCTRL1
    {0x0A, 0x00},  // CHANNR
    {0x0B, 0x0F},  // FSCTRL1
    {0x0C, 0x00},  // FSCTRL0
    {0x10, 0x88},  // MDMCFG4 : RX filter BW ~203 kHz, DRATE_E=8
    {0x11, 0x83},  // MDMCFG3 : DRATE_M -> ~9.6 kbps, keeps 300 us pulses sharp
    {0x12, 0x32},  // MDMCFG2 : ASK/OOK, no preamble/sync
    {0x13, 0x02},  // MDMCFG1
    {0x14, 0xF8},  // MDMCFG0
    {0x15, 0x37},  // DEVIATN
    {0x16, 0x07},  // MCSM2
    {0x17, 0x30},  // MCSM1   : stay in RX after a packet
    {0x18, 0x14},  // MCSM0   : auto calibrate on RX/TX
    {0x19, 0x76},  // FOCCFG
    {0x1A, 0x6C},  // BSCFG
    {0x1B, 0x03},  // AGCCTRL2
    {0x1C, 0x40},  // AGCCTRL1
    {0x1D, 0x91},  // AGCCTRL0
    {0x1E, 0x87},  // WORCTRL
    {0x1F, 0x6B},  // FREND1
    {0x20, 0xF8},  // FREND0
    {0x21, 0x56},  // FSCAL3
    {0x22, 0x11},  // FSCAL2
    {0x23, 0xAA},  // FSCAL1
    {0x24, 0x0B},  // FSCAL0
    {0x25, 0x31},  // RCCTRL1
    {0x26, 0x0D},  // RCCTRL0
    {0x27, 0x41},  // FSTEST
    {0x28, 0x00},  // PTEST
    {0x29, 0x59},  // AGCTEST
    {0x2A, 0x7F},  // TEST2
    {0x2B, 0x3F},  // TEST1
    {0x2C, 0x88},  // TEST0
};

// GDO2 as a static level, used to drive the RF_SW1 pin of the antenna switch.
// Measured on this module: 0x2E leaves the pin high-Z, 0x2F drives it low and
// 0x6F (0x2F with the invert bit) drives it high.  The datasheet/RadioLib
// values 0x2E / 0x6E would leave RF_SW1 floating, which keeps the antenna
// switch disconnected - that is why nothing was ever received.
constexpr uint8_t kGdo2Low  = 0x2F;
constexpr uint8_t kGdo2High = 0x6F;

// POWER_EN of the Cap: enables the 3.3 V DCDC that supplies the CC1101 (and the
// NFC controller).  On the Cardputer ADV this net sits on G3.
constexpr int kPowerEnablePin = 3;

// The Cap wiring is short but not impedance controlled; a conservative clock
// with generous setup/hold times is far more reliable than 1 MHz.
constexpr uint32_t kSpiHz = 250000;

// Strobe commands.
constexpr uint8_t kSres  = 0x30;
constexpr uint8_t kSrx   = 0x34;
constexpr uint8_t kStx   = 0x35;
constexpr uint8_t kSidle = 0x36;
constexpr uint8_t kScal  = 0x33;
constexpr uint8_t kSfrx  = 0x3A;

struct FreqRegs {
    uint8_t f2, f1, f0;
};

// 26 MHz crystal: freq_reg = f_hz * 65536 / 26e6.
FreqRegs regsFor(RfBand band)
{
    switch (band) {
        case RfBand::k315: return {0x0C, 0x1D, 0x8A};  // 315.00 MHz
        case RfBand::k433: return {0x10, 0xB0, 0x76};  // 433.92 MHz
        case RfBand::k868: return {0x21, 0x62, 0x76};  // 867.99 MHz
    }
    return {0x0C, 0x1D, 0x8A};
}

}  // namespace

uint8_t Cc1101::readReg(uint8_t addr)
{
    // 0xC0 = read + burst.  The burst bit is required for the status registers
    // (PARTNUM 0x30, VERSION 0x31, MARCSTATE ...), and is harmless for the
    // configuration registers we read back.
    SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(rfcfg::csPin, LOW);
    delayMicroseconds(10);
    SPI.transfer(addr | 0xC0);
    const uint8_t value = SPI.transfer(0x00);
    delayMicroseconds(5);
    digitalWrite(rfcfg::csPin, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
    return value;
}

void Cc1101::writeReg(uint8_t addr, uint8_t value)
{
    SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(rfcfg::csPin, LOW);
    delayMicroseconds(10);
    SPI.transfer(addr & 0x3F);
    SPI.transfer(value);
    delayMicroseconds(5);
    digitalWrite(rfcfg::csPin, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
}

void Cc1101::strobe(uint8_t command)
{
    SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(rfcfg::csPin, LOW);
    delayMicroseconds(10);
    SPI.transfer(command);
    delayMicroseconds(5);
    digitalWrite(rfcfg::csPin, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
}

bool Cc1101::begin()
{
    // Power the module: G3 drives POWER_EN of the on-board 3.3 V regulator.
    pinMode(kPowerEnablePin, OUTPUT);
    digitalWrite(kPowerEnablePin, HIGH);
    delay(100);

    // Deselect the NFC controller that lives on the same SPI bus.
    pinMode(rfcfg::nfcCsPin, OUTPUT);
    digitalWrite(rfcfg::nfcCsPin, HIGH);

    pinMode(rfcfg::csPin, OUTPUT);
    digitalWrite(rfcfg::csPin, HIGH);

    pinMode(rfcfg::sw0Pin, OUTPUT);
    digitalWrite(rfcfg::sw0Pin, LOW);

    SPI.begin(rfcfg::sckPin, rfcfg::misoPin, rfcfg::mosiPin, rfcfg::csPin);
    delay(20);

    // Identify the chip, retrying a few times in case the 3.3 V rail is still
    // ramping up.
    partnum_ = 0xFF;
    version_ = 0xFF;
    for (int attempt = 0; attempt < 5; ++attempt) {
        partnum_ = readReg(0x30);
        version_ = readReg(0x31);
        if (partnum_ == 0x00 && (version_ == 0x14 || version_ == 0x04)) break;
        delay(30);
    }

    const bool alive = (partnum_ == 0x00) && (version_ == 0x14 || version_ == 0x04);
    Serial.printf("[cc1101] partnum=0x%02X version=0x%02X %s\n", partnum_, version_,
                  alive ? "ok" : "NOT DETECTED");

    // Configure the chip even when identification failed: in its power-on state
    // GDO0 outputs a clock, which would flood the receiver with bogus pulses.
    strobe(kSres);
    delay(20);

    for (const auto& rv : kConfig) {
        writeReg(rv.reg, rv.val);
    }

    strobe(kSidle);
    strobe(kScal);  // calibrate once after the configuration
    delay(5);

    ready_ = alive;
    band_  = RfBand::k315;
    setBand(RfBand::k315);
    enterRx();  // put GDO0 back into asynchronous data output before RX starts
    return ready_;
}

bool Cc1101::setBand(RfBand band)
{
    band_ = band;

    // A plain retune (write FREQ + SCAL) left the receiver in a noisy state when
    // hopping to 433 - GDO0 chattered continuously and the receiver never
    // closed a frame.  Reset and re-apply the whole configuration instead, which
    // puts the chip in exactly the state it has after a clean power-on.
    strobe(kSres);
    delay(15);
    for (const auto& rv : kConfig) {
        writeReg(rv.reg, rv.val);
    }

    const FreqRegs f = regsFor(band);
    writeReg(0x0D, f.f2);
    writeReg(0x0E, f.f1);
    writeReg(0x0F, f.f0);

    // Antenna switch, truth table from the Cap CC1101 schematic:
    //   315 MHz: RF_SW0=0 RF_SW1=1
    //   433 MHz: RF_SW0=1 RF_SW1=0
    //   868 MHz: RF_SW0=1 RF_SW1=1
    // RF_SW0 is a plain GPIO, RF_SW1 comes from the CC1101 GDO2 pin.
    const bool sw0 = (band != RfBand::k315);
    const bool sw1 = (band != RfBand::k433);
    digitalWrite(rfcfg::sw0Pin, sw0 ? HIGH : LOW);
    writeReg(0x00, sw1 ? kGdo2High : kGdo2Low);

    // Calibrate the synthesizer for the new frequency.
    strobe(kSidle);
    strobe(kScal);
    delay(5);
    strobe(kSrx);

    return true;
}

void Cc1101::setFrequencyHz(uint32_t hz)
{
    // freq_reg = f * 2^16 / f_xtal, with a 26 MHz crystal.  64 bit math keeps
    // the intermediate product inside range.
    const uint32_t reg = static_cast<uint32_t>((static_cast<uint64_t>(hz) << 16) / 26000000ull);
    writeReg(0x0D, static_cast<uint8_t>((reg >> 16) & 0xFF));
    writeReg(0x0E, static_cast<uint8_t>((reg >> 8) & 0xFF));
    writeReg(0x0F, static_cast<uint8_t>(reg & 0xFF));
}

bool Cc1101::tuneTo(RfBand band, uint32_t hz)
{
    // Hopping between bands needs the full reset + reconfiguration (a plain
    // retune left the receiver noisy, see setBand); inside one band writing the
    // frequency and recalibrating is enough and much faster.
    if (band != band_) {
        setBand(band);
    }
    setFrequencyHz(hz);

    strobe(kSidle);
    strobe(kScal);
    delay(2);
    return enterRx();
}

bool Cc1101::enterRx()
{
    writeReg(0x02, 0x0D);  // GDO0 = async serial data output
    writeReg(0x08, 0x32);  // async serial mode
    strobe(kSidle);
    strobe(kSfrx);
    strobe(kSrx);
    return true;
}

void Cc1101::setPaTable(uint8_t offLevel, uint8_t onLevel)
{
    // PATABLE is written as a burst at address 0x3E.
    SPI.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));
    digitalWrite(rfcfg::csPin, LOW);
    delayMicroseconds(10);
    SPI.transfer(0x7E);
    SPI.transfer(offLevel);
    SPI.transfer(onLevel);
    delayMicroseconds(5);
    digitalWrite(rfcfg::csPin, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
}

bool Cc1101::enterTx()
{
    writeReg(0x08, 0x32);  // async serial mode, data taken from GDO0
    // OOK power table: index 0 = carrier off, index 1 = +10 dBm on 315 MHz.
    setPaTable(0x00, 0xC0);
    strobe(kSidle);
    strobe(kStx);
    return true;
}

void Cc1101::idle()
{
    strobe(kSidle);
}

int Cc1101::rssiDbm()
{
    // RSSI is a signed 8 bit value in 0.5 dB steps; the datasheet conversion is
    //   dBm = rssi/2 - 74      (rssi treated as signed)
    // Read it through the burst header so the status register actually answers.
    const int8_t raw = static_cast<int8_t>(readReg(0x34));
    return static_cast<int>(raw) / 2 - 74;
}
