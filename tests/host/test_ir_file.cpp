/*
 * Host tests for src/ir_file.{h,cpp}.
 *
 * Self-contained: prints PASS / FAIL <line>: <what> for every check and exits
 * non-zero when one failed.
 *
 * ir_protocol.cpp is deliberately NOT part of this link (ir_file.cpp only calls
 * the ir:: declarations from ir_protocol.h), so the handful of ir:: functions
 * used here are stubbed below, guarded by IR_FILE_TEST_HAVE_REAL_PROTOCOL so
 * the file can also be linked against the real ir_protocol.cpp.
 *
 * Build:
 *   g++ -std=c++17 -Wall -Wextra -I tests/host/stubs -I src \
 *       -o tests/host/test_ir_file.exe tests/host/test_ir_file.cpp src/ir_file.cpp
 */

#include <cstdio>
#include <cstring>

#include "ir_file.h"

// ---------------------------------------------------------------------------
// ir:: stubs (host link has no ir_protocol.cpp)
// ---------------------------------------------------------------------------
#ifndef IR_FILE_TEST_HAVE_REAL_PROTOCOL
namespace ir {
namespace {

struct ProtoName {
    const char* name;
    Protocol proto;
};

// Flipper spellings, matched case-insensitively by both directions.
const ProtoName kProtoNames[] = {
    {"NEC", Protocol::kNec},         {"NECEXT", Protocol::kNecExt},
    {"SAMSUNG32", Protocol::kSamsung32}, {"RC5", Protocol::kRc5},
    {"RC5X", Protocol::kRc5X},       {"RC6", Protocol::kRc6},
    {"SIRC", Protocol::kSirc},       {"SIRC15", Protocol::kSirc15},
    {"SIRC20", Protocol::kSirc20},   {"KASEIKYO", Protocol::kKaseikyo},
};

bool sameNoCase(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

}  // namespace

const char* protocolName(Protocol p) {
    for (int i = 0; i < static_cast<int>(sizeof(kProtoNames) / sizeof(kProtoNames[0])); ++i) {
        if (kProtoNames[i].proto == p) return kProtoNames[i].name;
    }
    return "UNKNOWN";
}

Protocol protocolFromName(const char* name) {
    if (name == nullptr) return Protocol::kUnknown;
    for (int i = 0; i < static_cast<int>(sizeof(kProtoNames) / sizeof(kProtoNames[0])); ++i) {
        if (sameNoCase(name, kProtoNames[i].name)) return kProtoNames[i].proto;
    }
    return Protocol::kUnknown;
}

bool protocolSupported(const char* name) {
    const Protocol p = protocolFromName(name);
    return p != Protocol::kUnknown && p != Protocol::kRaw;
}

bool encodable(Protocol p) {
    return p != Protocol::kUnknown && p != Protocol::kRaw;
}

}  // namespace ir
#endif  // IR_FILE_TEST_HAVE_REAL_PROTOCOL

// ---------------------------------------------------------------------------
// Check harness
// ---------------------------------------------------------------------------
static int g_failures = 0;
static int g_checks   = 0;

static void check(bool ok, int line, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("PASS %d: %s\n", line, what);
    } else {
        ++g_failures;
        std::printf("FAIL %d: %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), __LINE__, #cond)

// ---------------------------------------------------------------------------
// Test data (static storage: Entry is ~1.1 KB)
// ---------------------------------------------------------------------------
static irfile::Header g_headers[8];
static irfile::Entry g_entry;
static irfile::Entry g_entry2;
static irfile::Entry g_roundTrip;
static irfile::Entry g_entries[3];
static char g_out[8192];
static char g_out2[8192];
static char g_bigFile[16384];

// The documented Flipper example: 2 parsed + 1 raw button.
static const char kExample[] = R"IR(Filetype: IR signals file
Version: 1
#
name: Button_1
type: parsed
protocol: NECext
address: EE 87 00 00
command: 5D A0 00 00
#
name: Volume Up
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 504 3432 502 483 500 484 510 502 502 482 501 485 509 1452 504 1458 509 1452 504 481 501 474 509 3420 503
#
name: Button_3
type: parsed
protocol: NEC
address: 04 00 00 00
command: 08 00 00 00
#
)IR";

static const char kCrlf[] =
    "Filetype: IR signals file\r\n"
    "Version: 1\r\n"
    "#\r\n"
    "name: Crlf Button\r\n"
    "type: parsed\r\n"
    "protocol: Samsung32\r\n"
    "address: 07 00 00 00\r\n"
    "command: 02 00 00 00\r\n"
    "#\r\n";

// No Filetype/Version header, and no '#' between the two names either.
static const char kNoFiletype[] = R"IR(name: No Header
type: parsed
protocol: RC5
address: 10 00 00 00
command: 0C 00 00 00
name: Second
type: parsed
protocol: SIRC
address: 01 00 00 00
command: 15 00 00 00
)IR";

static const char kUnknownKeys[] = R"IR(Filetype: IR signals file
Version: 1
manufacturer: someone
#
name:   Spaced Name
type: parsed
protocol: NEC
address: 01 00 00 00
command: 02 00 00 00
extra: ignored
another junk line without a colon
)IR";

static const char kLowerKeys[] = R"IR(# lowercase keys
NAME: lower
TYPE: parsed
PROTOCOL: nec
ADDRESS: 03 00 00 00
COMMAND: 09 00 00 00
)IR";

static const char kLongName[] = R"IR(name: Very Long Button Name Indeed
type: parsed
protocol: NEC
address: 01 00 00 00
command: 02 00 00 00
)IR";

static const char kMalformedHex[] = R"IR(name: Broken
type: parsed
protocol: NEC
address: ZZ 87 00 00
command: 1FF
)IR";

static const char kUnsupported[] = R"IR(name: Rca Button
type: parsed
protocol: RCA
address: 01 00 00 00
command: 02 00 00 00
)IR";

static const char kOutOfRangeTimings[] = R"IR(name: Clamped Timings
type: raw
frequency: 38000
duty_cycle: 0.500000
data: 70000 0 65535 1234
)IR";

static const char kEmptyRaw[] = R"IR(name: Empty Raw
type: raw
frequency: 36000
duty_cycle: 0.250000
data:
)IR";

// 700 timings: the parser must clamp to ir::kMaxTimings.
static void buildBigRaw() {
    int used = std::snprintf(g_bigFile, sizeof g_bigFile,
                             "Filetype: IR signals file\nVersion: 1\n#\n"
                             "name: Big Raw\ntype: raw\nfrequency: 38000\n"
                             "duty_cycle: 0.500000\ndata:");
    static const int kTimings = 700;
    for (int i = 0; i < kTimings && used > 0; ++i) {
        const int room = static_cast<int>(sizeof g_bigFile) - used;
        if (room <= 8) break;
        const int n = std::snprintf(g_bigFile + used, static_cast<size_t>(room), " %d", 100 + i);
        if (n <= 0) break;
        used += n;
    }
    if (used > 0 && used < static_cast<int>(sizeof g_bigFile) - 1) {
        g_bigFile[used++] = '\n';
        g_bigFile[used]   = '\0';
    }
}

static int textLen(const char* s) { return static_cast<int>(std::strlen(s)); }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
static void testExampleList() {
    const int n = irfile::listButtons(kExample, textLen(kExample), g_headers, 8);
    CHECK(n == 3);
    CHECK(std::strcmp(g_headers[0].name, "Button_1") == 0);
    CHECK(g_headers[0].raw == false);
    CHECK(std::strcmp(g_headers[1].name, "Volume Up") == 0);
    CHECK(g_headers[1].raw == true);
    CHECK(std::strcmp(g_headers[2].name, "Button_3") == 0);
    CHECK(g_headers[2].raw == false);
}

static void testExampleLoad() {
    CHECK(irfile::loadButton(kExample, textLen(kExample), 0, &g_entry));
    CHECK(g_entry.raw == false);
    CHECK(g_entry.signal.proto == ir::Protocol::kNecExt);
    CHECK(g_entry.signal.address == 0x000087EEu);
    CHECK(g_entry.signal.command == 0x0000A05Du);
    CHECK(g_entry.count == 0);

    CHECK(irfile::loadButton(kExample, textLen(kExample), 1, &g_entry));
    CHECK(g_entry.raw == true);
    CHECK(g_entry.frequency == 38000u);
    CHECK(g_entry.dutyPercent == 33);
    CHECK(g_entry.count == 25);
    CHECK(g_entry.timings[0] == 504);
    CHECK(g_entry.timings[1] == 3432);
    CHECK(g_entry.timings[24] == 503);

    CHECK(irfile::loadButton(kExample, textLen(kExample), 2, &g_entry));
    CHECK(g_entry.signal.proto == ir::Protocol::kNec);
    CHECK(g_entry.signal.address == 4u);
    CHECK(g_entry.signal.command == 8u);
}

static void testCrlf() {
    CHECK(irfile::listButtons(kCrlf, textLen(kCrlf), g_headers, 8) == 1);
    CHECK(std::strcmp(g_headers[0].name, "Crlf Button") == 0);
    CHECK(irfile::loadButton(kCrlf, textLen(kCrlf), 0, &g_entry));
    CHECK(g_entry.signal.proto == ir::Protocol::kSamsung32);
    CHECK(g_entry.signal.address == 7u);
    CHECK(g_entry.signal.command == 2u);
}

static void testNoFiletype() {
    CHECK(irfile::listButtons(kNoFiletype, textLen(kNoFiletype), g_headers, 8) == 2);
    CHECK(std::strcmp(g_headers[0].name, "No Header") == 0);
    CHECK(std::strcmp(g_headers[1].name, "Second") == 0);
    CHECK(irfile::loadButton(kNoFiletype, textLen(kNoFiletype), 0, &g_entry));
    CHECK(g_entry.signal.proto == ir::Protocol::kRc5);
    CHECK(g_entry.signal.address == 0x10u);
    CHECK(g_entry.signal.command == 0x0Cu);
    CHECK(irfile::loadButton(kNoFiletype, textLen(kNoFiletype), 1, &g_entry2));
    CHECK(g_entry2.signal.proto == ir::Protocol::kSirc);
}

static void testUnknownAndSpaces() {
    CHECK(irfile::listButtons(kUnknownKeys, textLen(kUnknownKeys), g_headers, 8) == 1);
    CHECK(std::strcmp(g_headers[0].name, "Spaced Name") == 0);  // trimmed
    CHECK(irfile::loadButton(kUnknownKeys, textLen(kUnknownKeys), 0, &g_entry));
    CHECK(g_entry.signal.proto == ir::Protocol::kNec);
    CHECK(g_entry.signal.address == 1u);
    CHECK(g_entry.signal.command == 2u);

    // Lowercase keys and lowercase protocol spelling.
    CHECK(irfile::listButtons(kLowerKeys, textLen(kLowerKeys), g_headers, 8) == 1);
    CHECK(std::strcmp(g_headers[0].name, "lower") == 0);
    CHECK(irfile::loadButton(kLowerKeys, textLen(kLowerKeys), 0, &g_entry));
    CHECK(g_entry.signal.proto == ir::Protocol::kNec);
    CHECK(g_entry.signal.command == 9u);

    // Over-long names are truncated to 23 characters + NUL.
    CHECK(irfile::listButtons(kLongName, textLen(kLongName), g_headers, 8) == 1);
    CHECK(static_cast<int>(std::strlen(g_headers[0].name)) == 23);
    CHECK(std::strcmp(g_headers[0].name, "Very Long Button Name I") == 0);
}

static void testClamping() {
    buildBigRaw();
    CHECK(irfile::listButtons(g_bigFile, textLen(g_bigFile), g_headers, 8) == 1);
    CHECK(g_headers[0].raw == true);
    CHECK(irfile::loadButton(g_bigFile, textLen(g_bigFile), 0, &g_entry));
    CHECK(g_entry.raw == true);
    CHECK(g_entry.count == ir::kMaxTimings);
    CHECK(g_entry.timings[0] == 100);
    CHECK(g_entry.timings[ir::kMaxTimings - 1] == 611);

    // Out of uint16 range values clamp instead of wrapping.
    CHECK(irfile::loadButton(kOutOfRangeTimings, textLen(kOutOfRangeTimings), 0, &g_entry));
    CHECK(g_entry.count == 4);
    CHECK(g_entry.timings[0] == 65535);
    CHECK(g_entry.timings[1] == 0);
    CHECK(g_entry.timings[2] == 65535);
    CHECK(g_entry.timings[3] == 1234);

    // A raw record with no timings is still a valid, loadable entry.
    CHECK(irfile::loadButton(kEmptyRaw, textLen(kEmptyRaw), 0, &g_entry));
    CHECK(g_entry.raw == true);
    CHECK(g_entry.count == 0);
    CHECK(g_entry.frequency == 36000u);
    CHECK(g_entry.dutyPercent == 25);
}

static void testMalformed() {
    // Malformed hex: byte 0 is junk -> 0, so "ZZ 87 00 00" is 0x00008700;
    // "1FF" is one over-wide byte and clamps to 0xFF.
    CHECK(irfile::listButtons(kMalformedHex, textLen(kMalformedHex), g_headers, 8) == 1);
    CHECK(irfile::loadButton(kMalformedHex, textLen(kMalformedHex), 0, &g_entry));
    CHECK(g_entry.signal.address == 0x00008700u);
    CHECK(g_entry.signal.command == 0xFFu);

    // Unsupported protocol: entry is kept, proto is kUnknown, load says false.
    CHECK(irfile::listButtons(kUnsupported, textLen(kUnsupported), g_headers, 8) == 1);
    CHECK(irfile::loadButton(kUnsupported, textLen(kUnsupported), 0, &g_entry) == false);
    CHECK(g_entry.signal.proto == ir::Protocol::kUnknown);
    CHECK(g_entry.raw == false);

    // Not an IR file at all.
    static const char kGarbage[] = "hello world\nthis is not an ir file\n";
    CHECK(irfile::listButtons(kGarbage, textLen(kGarbage), g_headers, 8) == -1);
    CHECK(irfile::listButtons("", 0, g_headers, 8) == -1);
    CHECK(irfile::listButtons("   \n\n\t\n", 7, g_headers, 8) == -1);
    CHECK(irfile::listButtons(nullptr, 10, g_headers, 8) == -1);
    CHECK(irfile::loadButton("", 0, 0, &g_entry) == false);
    CHECK(irfile::loadButton(kGarbage, textLen(kGarbage), 0, &g_entry) == false);
    CHECK(irfile::loadButton(kExample, 0, 0, &g_entry) == false);  // zero length
    CHECK(irfile::loadButton(kExample, textLen(kExample), 0, nullptr) == false);
}

static void testRoundTripParsed() {
    g_entries[0]                             = irfile::Entry();
    std::strcpy(g_entries[0].name, "NEC Ext");
    g_entries[0].raw                         = false;
    g_entries[0].signal.proto                = ir::Protocol::kNecExt;
    g_entries[0].signal.address              = 0x000087EEu;
    g_entries[0].signal.command              = 0x0000A05Du;
    g_entries[0].signal.toggle               = false;
    g_entries[0].signal.repeat               = false;
    g_entries[0].count                       = 0;

    const int n = irfile::formatFile(g_entries, 1, g_out, static_cast<int>(sizeof g_out));
    CHECK(n > 0);
    CHECK(std::strstr(g_out, "Filetype: IR signals file") != nullptr);
    CHECK(std::strstr(g_out, "Version: 1") != nullptr);
    CHECK(std::strstr(g_out, "type: parsed") != nullptr);
    CHECK(std::strstr(g_out, "address: EE 87 00 00") != nullptr);
    CHECK(std::strstr(g_out, "command: 5D A0 00 00") != nullptr);

    CHECK(irfile::listButtons(g_out, n, g_headers, 8) == 1);
    CHECK(std::strcmp(g_headers[0].name, "NEC Ext") == 0);
    CHECK(irfile::loadButton(g_out, n, 0, &g_roundTrip));
    CHECK(g_roundTrip.raw == false);
    CHECK(g_roundTrip.signal.proto == ir::Protocol::kNecExt);
    CHECK(g_roundTrip.signal.address == 0x000087EEu);
    CHECK(g_roundTrip.signal.command == 0x0000A05Du);
    CHECK(g_roundTrip.count == 0);
}

static void testRoundTripRaw() {
    g_entries[1]                 = irfile::Entry();
    std::strcpy(g_entries[1].name, "Raw One");
    g_entries[1].raw             = true;
    g_entries[1].frequency       = 36000;
    g_entries[1].dutyPercent     = 33;
    g_entries[1].count           = 4;
    g_entries[1].timings[0]      = 504;
    g_entries[1].timings[1]      = 3432;
    g_entries[1].timings[2]      = 502;
    g_entries[1].timings[3]      = 483;

    const int n = irfile::formatFile(g_entries + 1, 1, g_out, static_cast<int>(sizeof g_out));
    CHECK(n > 0);
    CHECK(std::strstr(g_out, "type: raw") != nullptr);
    CHECK(std::strstr(g_out, "duty_cycle: 0.330000") != nullptr);
    CHECK(std::strstr(g_out, "data: 504 3432 502 483") != nullptr);

    CHECK(irfile::loadButton(g_out, n, 0, &g_roundTrip));
    CHECK(g_roundTrip.raw == true);
    CHECK(g_roundTrip.frequency == 36000u);
    CHECK(g_roundTrip.dutyPercent == 33);
    CHECK(g_roundTrip.count == 4);
    CHECK(g_roundTrip.timings[0] == 504 && g_roundTrip.timings[3] == 483);

    // Formatting the loaded entry again must be byte identical.
    const int n2 = irfile::formatFile(&g_roundTrip, 1, g_out2, static_cast<int>(sizeof g_out2));
    CHECK(n2 == n);
    CHECK(std::strcmp(g_out, g_out2) == 0);
}

static void testCapsAndRanges() {
    const int len = textLen(kExample);
    CHECK(irfile::listButtons(kExample, len, g_headers, 2) == 2);  // capped
    CHECK(irfile::listButtons(kExample, len, g_headers, 1) == 1);
    CHECK(irfile::listButtons(kExample, len, g_headers, 0) == 0);
    CHECK(irfile::listButtons(kExample, len, nullptr, 3) == 3);  // count only

    CHECK(irfile::loadButton(kExample, len, 3, &g_entry) == false);
    CHECK(irfile::loadButton(kExample, len, -1, &g_entry) == false);
    CHECK(irfile::loadButton(kExample, len, 200, &g_entry) == false);

    // Buffer too small for even the header.
    CHECK(irfile::formatFile(g_entries, 1, g_out, 10) == -1);
    CHECK(irfile::formatFile(g_entries, 1, g_out, 0) == -1);
    CHECK(irfile::formatFile(g_entries, 1, nullptr, 0) == -1);

    // Zero entries: header only, and still a parseable "file".
    const int n = irfile::formatFile(nullptr, 0, g_out, static_cast<int>(sizeof g_out));
    CHECK(n > 0);
    CHECK(irfile::listButtons(g_out, n, g_headers, 8) == 0);
}

static void testMultipleEntries() {
    g_entries[2]                 = irfile::Entry();
    std::strcpy(g_entries[2].name, "Third");
    g_entries[2].raw             = false;
    g_entries[2].signal.proto    = ir::Protocol::kKaseikyo;
    g_entries[2].signal.address  = 0x0000E100u;
    g_entries[2].signal.command  = 0x00009F60u;

    const int n = irfile::formatFile(g_entries, 3, g_out, static_cast<int>(sizeof g_out));
    CHECK(n > 0);
    CHECK(irfile::listButtons(g_out, n, g_headers, 8) == 3);
    CHECK(std::strcmp(g_headers[2].name, "Third") == 0);
    CHECK(irfile::loadButton(g_out, n, 2, &g_roundTrip));
    CHECK(g_roundTrip.signal.proto == ir::Protocol::kKaseikyo);
    CHECK(g_roundTrip.signal.address == 0x0000E100u);
    CHECK(g_roundTrip.signal.command == 0x00009F60u);
    CHECK(ir::encodable(g_roundTrip.signal.proto) == true);
}

static void testFormatEntry() {
    // Single parsed button block, used by ir_app to append to a hand written
    // file header (the library export path).
    const int n =
        irfile::formatEntry(g_entries[0].signal, "NEC Ext", g_out, static_cast<int>(sizeof g_out));
    CHECK(n > 0);
    CHECK(std::strncmp(g_out, "#\n", 2) == 0);
    CHECK(std::strstr(g_out, "name: NEC Ext") != nullptr);
    CHECK(std::strstr(g_out, "type: parsed") != nullptr);
    CHECK(std::strstr(g_out, "address: EE 87 00 00") != nullptr);
    CHECK(std::strstr(g_out, "Filetype") == nullptr);  // no file header
    CHECK(irfile::formatEntry(g_entries[0].signal, "NEC Ext", g_out, 8) == -1);
    CHECK(irfile::formatEntry(g_entries[0].signal, "NEC Ext", nullptr, 0) == -1);
    CHECK(irfile::formatEntry(g_entries[0].signal, nullptr, g_out, static_cast<int>(sizeof g_out)) >
          0);

    // Header + two appended blocks parses back as a normal two button file.
    int used = std::snprintf(g_out2, sizeof g_out2, "Filetype: IR signals file\nVersion: 1\n");
    const int e1 = irfile::formatEntry(g_entries[0].signal, g_entries[0].name, g_out2 + used,
                                       static_cast<int>(sizeof g_out2) - used);
    CHECK(e1 > 0);
    if (e1 > 0) used += e1;
    const int e2 = irfile::formatEntry(g_entries[2].signal, g_entries[2].name, g_out2 + used,
                                       static_cast<int>(sizeof g_out2) - used);
    CHECK(e2 > 0);
    if (e2 > 0) used += e2;
    CHECK(irfile::listButtons(g_out2, used, g_headers, 8) == 2);
    CHECK(irfile::loadButton(g_out2, used, 1, &g_roundTrip));
    CHECK(g_roundTrip.signal.proto == ir::Protocol::kKaseikyo);
    CHECK(g_roundTrip.signal.address == 0x0000E100u);
    CHECK(g_roundTrip.signal.command == 0x00009F60u);
}

int main() {
    testExampleList();
    testExampleLoad();
    testCrlf();
    testNoFiletype();
    testUnknownAndSpaces();
    testClamping();
    testMalformed();
    testRoundTripParsed();
    testRoundTripRaw();
    testCapsAndRanges();
    testMultipleEntries();
    testFormatEntry();

    std::printf("%s: %d checks, %d failure(s)\n", g_failures == 0 ? "ALL PASS" : "FAILED",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
