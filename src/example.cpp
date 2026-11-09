// Diagnostic: 433 MHz receiver state across the four antenna-switch combinations.
#include <M5Unified.h>
#include <SPI.h>

namespace {
constexpr int kCs = 5;

uint8_t rd(uint8_t addr)
{
    SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    digitalWrite(kCs, LOW);
    delayMicroseconds(10);
    SPI.transfer(addr | 0xC0);
    const uint8_t v = SPI.transfer(0);
    delayMicroseconds(5);
    digitalWrite(kCs, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
    return v;
}

void wr(uint8_t addr, uint8_t val)
{
    SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    digitalWrite(kCs, LOW);
    delayMicroseconds(10);
    SPI.transfer(addr & 0x3F);
    SPI.transfer(val);
    delayMicroseconds(5);
    digitalWrite(kCs, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
}

void strobe(uint8_t cmd)
{
    SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
    digitalWrite(kCs, LOW);
    delayMicroseconds(10);
    SPI.transfer(cmd);
    delayMicroseconds(5);
    digitalWrite(kCs, HIGH);
    SPI.endTransaction();
    delayMicroseconds(5);
}

int countEdges(uint32_t ms)
{
    int last  = digitalRead(15);
    int edges = 0;
    const uint32_t t0 = micros();
    while (micros() - t0 < ms * 1000u) {
        const int v = digitalRead(15);
        if (v != last) { edges++; last = v; }
    }
    return edges;
}

struct Combo { int sw0; uint8_t gdo2; const char* name; };
const Combo kCombos[] = {
    {1, 0x2F, "433 sw0=1 sw1=0 (schematic)"},
    {0, 0x6F, "433 sw0=0 sw1=1 (doc)"},
    {0, 0x2F, "433 sw0=0 sw1=0"},
    {1, 0x6F, "433 sw0=1 sw1=1"},
};
}  // namespace

void setup()
{
    Serial.begin(115200);
    delay(200);
    M5.begin();
    M5.Display.setRotation(0);
    M5.Display.setBrightness(80);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_GREEN);

    pinMode(3, OUTPUT); digitalWrite(3, HIGH);
    pinMode(6, OUTPUT); digitalWrite(6, HIGH);
    pinMode(12, OUTPUT); digitalWrite(12, HIGH);
    pinMode(5, OUTPUT); digitalWrite(5, HIGH);
    pinMode(13, OUTPUT);
    pinMode(15, INPUT);
    delay(150);

    SPI.begin(40, 39, 14, kCs);
    delay(10);
    Serial.printf("id part=0x%02X ver=0x%02X\n", rd(0x30), rd(0x31));

    const uint8_t cfg[][2] = {
        {0x03, 0x0F}, {0x04, 0x12}, {0x05, 0xAD}, {0x06, 0xFF}, {0x07, 0x64},
        {0x0A, 0x00}, {0x0B, 0x0F}, {0x0C, 0x00}, {0x10, 0x88}, {0x11, 0x83},
        {0x12, 0x32}, {0x13, 0x02}, {0x14, 0xF8}, {0x15, 0x37}, {0x16, 0x07},
        {0x17, 0x30}, {0x18, 0x14}, {0x19, 0x76}, {0x1A, 0x6C}, {0x1B, 0x03},
        {0x1C, 0x40}, {0x1D, 0x91}, {0x1E, 0x87}, {0x1F, 0x6B}, {0x20, 0xF8},
        {0x21, 0x56}, {0x22, 0x11}, {0x23, 0xAA}, {0x24, 0x0B}, {0x25, 0x31},
        {0x26, 0x0D}, {0x27, 0x41}, {0x28, 0x00}, {0x29, 0x59}, {0x2A, 0x7F},
        {0x2B, 0x3F}, {0x2C, 0x88},
    };
    for (const auto& rv : cfg) wr(rv[0], rv[1]);

    strobe(0x36);  // SIDLE
    strobe(0x30);  // SRES
    delay(20);
    for (const auto& rv : cfg) wr(rv[0], rv[1]);

    // 433.92 MHz
    wr(0x0D, 0x10); wr(0x0E, 0xB0); wr(0x0F, 0x76);
    wr(0x02, 0x0D); wr(0x08, 0x32);
}

void loop()
{
    static int idx = 0;
    const Combo& c = kCombos[idx];

    digitalWrite(13, c.sw0 ? HIGH : LOW);
    wr(0x00, c.gdo2);
    strobe(0x36);
    strobe(0x33);
    delay(5);
    strobe(0x34);
    delay(5);

    const int edges = countEdges(200);
    Serial.printf("%-28s marc=0x%02X rssi=0x%02X edges200=%d\n", c.name, rd(0x35) & 0x1F,
                  rd(0x34), edges);
    M5.Display.setCursor(0, 20 + idx * 12);
    M5.Display.printf("%s e=%d", c.name, edges);

    idx = (idx + 1) % 4;
    delay(500);
}