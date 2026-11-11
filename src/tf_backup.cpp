#include "tf_backup.h"

#include "sd_mount.h"

#include <SD.h>
#include <SPI.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kSdCsPin        = 12;
constexpr uint32_t kSdHz      = 10000000;
constexpr const char* kDir    = "/cardputer";
constexpr const char* kPath   = "/cardputer/signals.txt";
constexpr const char* kTmp    = "/cardputer/signals.tmp";
constexpr const char* kHeader = "CardputerSig 1";

// The card is only mounted for the duration of one backup / restore, so the
// CC1101 keeps the SPI bus to itself while it is receiving.
bool mount()
{
    // One shared implementation: asks the board for its SD pins, retries at
    // decreasing clock speeds and logs every attempt.
    if (SD.cardType() != CARD_NONE) return true;
    const uint32_t t0 = millis();
    const bool ok     = sdmount::ensure();
    Serial.printf("[tfm] ensure=%d dt=%lums\n", static_cast<int>(ok),
                  static_cast<unsigned long>(millis() - t0));
    return ok;
}
void unmount()
{
    SD.end();
}

bool writeSignal(File& file, const SavedSignal& signal)
{
    char name[sizeof(signal.name)];
    snprintf(name, sizeof(name), "%s", signal.name);
    for (char* p = name; *p != 0; ++p) {
        if (*p == '\r' || *p == '\n') *p = ' ';
    }
    if (name[0] == 0) snprintf(name, sizeof(name), "SIG");

    int count = signal.count;
    if (count > rfcfg::storedPulses) count = rfcfg::storedPulses;
    if (count <= 0) return false;

    if (file.printf("S %s %u %u %06lX %u %d %s\n", rfcfg::bandName(signal.band),
                    static_cast<unsigned>(signal.bits), static_cast<unsigned>(signal.proto),
                    static_cast<unsigned long>(signal.code & 0xFFFFFFu),
                    static_cast<unsigned>(signal.baseUs), count, name) == 0) {
        return false;
    }

    if (file.print("P") == 0) return false;
    for (int i = 0; i < count; ++i) {
        if (file.printf(" %u", static_cast<unsigned>(signal.pulses[i])) == 0) return false;
    }
    return file.print('\n') == 1;
}

// Restore target lives in .bss: eight SavedSignal records would be ~2.7 kB of
// task stack.
SavedSignal g_items[rfcfg::maxSaved];

}  // namespace

namespace tfbackup {

TfResult saveSignals(const SavedStore& store, int* count)
{
    if (count) *count = 0;
    const uint32_t t0 = millis();
    Serial.printf("[tfs] save start n=%d\n", store.size());
    if (!mount()) return TfResult::kNoCard;

    if (!SD.exists(kDir)) SD.mkdir(kDir);

    File file = SD.open(kTmp, FILE_WRITE);
    if (!file) {
        unmount();
        return TfResult::kIoError;
    }

    bool ok      = file.printf("%s\n", kHeader) > 0;
    int written  = 0;
    for (int i = 0; ok && i < store.size(); ++i) {
        ok = writeSignal(file, store.at(i));
        if (ok) ++written;
    }
    file.flush();
    file.close();

    if (ok) {
        SD.remove(kPath);
        ok = SD.rename(kTmp, kPath);
    }
    if (!ok) SD.remove(kTmp);

    unmount();
    if (count) *count = written;
    Serial.printf("[tfs] save done ok=%d n=%d dt=%lums\n", ok, written,
                  static_cast<unsigned long>(millis() - t0));
    return ok ? TfResult::kOk : TfResult::kIoError;
}

TfResult loadSignals(SavedStore& store, int* count)
{
    if (count) *count = 0;
    const uint32_t t0 = millis();
    Serial.printf("[tfs] load start\n");
    if (!mount()) return TfResult::kNoCard;

    File file = SD.open(kPath, FILE_READ);
    if (!file) {
        unmount();
        return TfResult::kNoFile;
    }

    String header = file.readStringUntil('\n');
    header.trim();
    if (header != kHeader) {
        file.close();
        unmount();
        return TfResult::kBadFile;
    }

    int loaded   = 0;
    bool skipped = false;
    while (file.available() && loaded < rfcfg::maxSaved) {
        String head = file.readStringUntil('\n');
        head.trim();
        if (!head.startsWith("S ")) continue;

        const String line = file.readStringUntil('\n');

        unsigned mhz = 0, bits = 0, proto = 0, baseUs = 0, pulses = 0;
        unsigned long code                  = 0;
        char name[sizeof(g_items[0].name)] = {0};
        const int fields =
            sscanf(head.c_str(), "S %u %u %u %lx %u %u %15[^\n]", &mhz, &bits, &proto, &code,
                   &baseUs, &pulses, name);

        RfBand band;
        if (mhz == 315) {
            band = RfBand::k315;
        } else if (mhz == 433) {
            band = RfBand::k433;
        } else if (mhz == 868) {
            band = RfBand::k868;
        } else {
            skipped = true;
            continue;
        }

        if (fields < 6 || bits > 32 || baseUs < 10 || baseUs > 20000 || pulses == 0 ||
            pulses > rfcfg::storedPulses || !line.startsWith("P")) {
            skipped = true;
            continue;
        }

        SavedSignal& item = g_items[loaded];
        item              = SavedSignal();

        const char* cursor = line.c_str() + 1;
        int got            = 0;
        bool pulsesOk      = true;
        while (got < static_cast<int>(pulses)) {
            char* end        = nullptr;
            const long value = strtol(cursor, &end, 10);
            if (end == cursor || value < 1 || value > 60000) {
                pulsesOk = false;
                break;
            }
            item.pulses[got++] = static_cast<uint16_t>(value);
            cursor             = end;
        }
        if (!pulsesOk || got != static_cast<int>(pulses)) {
            skipped = true;
            continue;
        }

        item.band   = band;
        item.bits   = static_cast<uint8_t>(bits);
        item.proto  = static_cast<uint8_t>(proto);
        item.code   = static_cast<uint32_t>(code) & 0xFFFFFFu;
        item.baseUs = static_cast<uint16_t>(baseUs);
        item.count  = static_cast<uint16_t>(pulses);
        if (name[0] != 0) {
            snprintf(item.name, sizeof(item.name), "%s", name);
        } else {
            snprintf(item.name, sizeof(item.name), "%06lX",
                     static_cast<unsigned long>(item.code));
        }
        ++loaded;
    }

    file.close();
    unmount();

    if (loaded == 0) return skipped ? TfResult::kBadFile : TfResult::kEmpty;

    store.replaceAll(g_items, loaded);
    if (count) *count = loaded;
    Serial.printf("[tfs] load done n=%d skip=%d dt=%lums\n", loaded, skipped,
                  static_cast<unsigned long>(millis() - t0));
    return skipped ? TfResult::kPartial : TfResult::kOk;
}

}  // namespace tfbackup
