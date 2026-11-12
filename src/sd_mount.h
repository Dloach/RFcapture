// Shared SD mount for the two features (RF backup, NFC card backup).
//
// Both features share the same SPI bus and the same card slot, so the mount logic
// lives in one place.  It asks M5Unified which pins the board uses rather than
// trusting a constant, tries them in turn, and drops the clock on every retry --
// SD initialisation is specified to run at 400 kHz and a long shared bus is not
// always happy at 10 MHz.
#pragma once

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>

namespace sdmount {

inline int8_t boardCs()
{
#if defined(SD_SPI_CS)
    return SD_SPI_CS;
#else
    return M5.getPin(m5::pin_name_t::sd_spi_cs);
#endif
}

// Mounts the card if it is not already mounted.  Returns true when a card is
// usable.  Prints one line per attempt: on hardware problems the print is the
// only evidence there is.
inline bool ensure(const bool cutRadioPower = false)
{
    if (SD.cardType() != CARD_NONE) return true;

    const int8_t cs = boardCs();
    static bool reported = false;
    if (!reported) {
        reported = true;
        Serial.printf("[sd] pins cs=%d sck=%d miso=%d mosi=%d\n", static_cast<int>(cs),
                      M5.getPin(m5::pin_name_t::sd_spi_sclk),
                      M5.getPin(m5::pin_name_t::sd_spi_miso),
                      M5.getPin(m5::pin_name_t::sd_spi_mosi));
    }

    // Deselect the two radio chips while the card is talked to.  main.cpp does
    // the same at boot; repeat it here because the deferred NFC-side SD actions
    // (nfccards::runPendingAction) reach this function through a different path.
    //
    // Never cut module power (POWER_EN, GPIO 3) to do this: while the module is
    // unpowered the live SPI pins feed its chips through their pin clamp diodes,
    // the rail never collapses, and the chips come back half-powered and deaf
    // for the rest of the power cycle.  Explicitly driving both CS lines high
    // is enough: a deselected ST25R3916 tri-states its MISO.
    pinMode(5, OUTPUT);   // CC1101 CS
    digitalWrite(5, HIGH);
    pinMode(6, OUTPUT);   // ST25R3916 CS
    digitalWrite(6, HIGH);

    // The bus has to exist with the board's own pins before the card can be
    // talked to.  In the RF feature the CC1101 driver happens to call SPI.begin()
    // first, and the SD mount inherited that; a build that touches no radio (the
    // BACKUP feature) has nobody to do it, and SD.begin() alone was not enough --
    // every attempt timed out after 601 ms with cardType 0.
    SPI.begin(static_cast<int8_t>(M5.getPin(m5::pin_name_t::sd_spi_sclk)),
              static_cast<int8_t>(M5.getPin(m5::pin_name_t::sd_spi_miso)),
              static_cast<int8_t>(M5.getPin(m5::pin_name_t::sd_spi_mosi)),
              static_cast<int8_t>(cs < 0 ? 12 : cs));
    delay(10);

    // Soft-reset the ST25R3916 before touching the card.
    //
    // The chip keeps its registers across an ESP32 restart, and the NFC reader
    // configuration ends with the 13.56 MHz carrier ON (nfc_initial_field_on).
    // A live field on the shared flex is what corrupts the card's init -- the
    // exact failure is four 601 ms timeouts at every clock speed.  A power
    // pulse on POWER_EN also clears it but phantom-feeds the chip through the
    // live SPI pins, which kills the NFC feature for the rest of the power
    // cycle.  CMD_SET_DEFAULT puts the chip back to its power-up state
    // (carrier off) with two bytes on the bus; the NFC feature reconfigures
    // everything from scratch on its own begin(), so nothing is lost.
    //
    // Raw framing, SPI mode 1 (the ST25R3916 is the only mode-1 device here):
    // first byte OP_READ_REGISTER(0x40)|reg / OP_WRITE_REGISTER(0x00)|reg, then
    // CMD_SET_DEFAULT(0xC1) as a direct command.  An absent cap answers 0xFF
    // and is skipped.
    {
        pinMode(6, OUTPUT);
        uint8_t opctl = 0xFF;
        SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
        digitalWrite(6, LOW);
        SPI.transfer(0x40 | 0x02);  // read REG_OPERATION_CONTROL
        opctl = SPI.transfer(0x00);
        digitalWrite(6, HIGH);
        if (opctl != 0xFF) {
            digitalWrite(6, LOW);
            SPI.transfer(0xC1);  // CMD_SET_DEFAULT: power-up state, carrier off
            digitalWrite(6, HIGH);
        }
        SPI.endTransaction();
        Serial.printf("[sd] st25r opctl=%02X %s\n", opctl,
                      opctl == 0xFF ? "(no chip)" : "-> SET_DEFAULT");
        delay(5);
    }

    static const uint32_t kFreqs[] = {10000000, 4000000, 1000000, 400000};
    for (uint32_t f : kFreqs) {
        SD.end();
        const uint32_t t0 = millis();
        const bool ok     = SD.begin(static_cast<uint8_t>(cs < 0 ? 12 : cs), SPI, f);
        Serial.printf("[sd] begin(cs=%d,%lu)=%d type=%d dt=%lums\n", static_cast<int>(cs),
                      static_cast<unsigned long>(f), static_cast<int>(ok),
                      static_cast<int>(SD.cardType()),
                      static_cast<unsigned long>(millis() - t0));
        if (ok) return true;
        delay(20);
    }
    SD.end();
    return false;
}

// Restore module power (GPIO 3).  Anything that cut it must call this before the
// radio is used again -- the module's chips lose their configuration when their
// supply goes away.
inline void radioPowerOn()
{
    pinMode(3, OUTPUT);
    digitalWrite(3, HIGH);
    delay(120);
}

}  // namespace sdmount
