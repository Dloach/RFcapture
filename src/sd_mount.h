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

    // Power the radio module down first.
    //
    // CAP.CC1101 puts the CC1101 *and* the ST25R3916 behind one POWER_EN line
    // (GPIO 3), and both share SCK/MOSI/MISO with the SD slot.  A powered
    // ST25R3916 keeps driving MISO, so the card cannot answer and every mount
    // attempt dies on its 601 ms timeout -- and because the chip stays powered
    // across a reboot, only unplugging the board cleared it.  Cutting POWER_EN is
    // the documented way to share this bus: the other device leaves the bus before
    // the card is spoken to.
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
