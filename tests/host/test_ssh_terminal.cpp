// Host tests for the pure VT100/UTF-8/history logic in ssh_terminal.cpp.
// Build with:
//   g++ -std=c++17 -O2 -Wall -Itests/host/stubs -Isrc
//       tests/host/test_ssh_terminal.cpp src/ssh_terminal.cpp -o test_ssh_terminal

#include "ssh_terminal.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void expect(bool ok, const char* what)
{
    if (ok) {
        std::printf("  [ok]   %s\n", what);
    } else {
        std::printf("  [FAIL] %s\n", what);
        ++failures;
    }
}

std::string rowText(const sshterm::Cell row[sshterm::kCols])
{
    std::string s;
    for (int c = 0; c < sshterm::kCols; ++c) s.push_back(row[c].ch);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void testPlainText()
{
    sshterm::clear();
    const char text[] = "hello\r\nworld";
    sshterm::feed(text, sizeof(text) - 1);
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 0);
    expect(rowText(snap[0]) == "hello", "plain text row 0");
    expect(rowText(snap[1]) == "world", "plain text row 1");
}

void testSgrColor()
{
    sshterm::clear();
    const char text[] = "\x1b[31mR\x1b[0mN";
    sshterm::feed(text, sizeof(text) - 1);
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 0);
    expect(snap[0][0].ch == 'R' && snap[0][0].fg == 1, "SGR 31 -> red cell");
    expect(snap[0][1].ch == 'N' && snap[0][1].fg == 7, "SGR 0 -> default color");
}

void testUtf8Box()
{
    sshterm::clear();
    const char text[] = "\xE2\x94\x80";  // U+2500 box drawing horizontal
    sshterm::feed(text, sizeof(text) - 1);
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 0);
    expect(snap[0][0].ch == '-', "UTF-8 box drawing -> ASCII '-'");
}

void testEraseModes()
{
    sshterm::clear();
    const char text[] = "AAA\r\nBBB";
    sshterm::feed(text, sizeof(text) - 1);
    const char cup[] = "\x1b[2;1H";
    sshterm::feed(cup, sizeof(cup) - 1);
    const char erase0[] = "\x1b[J";
    sshterm::feed(erase0, sizeof(erase0) - 1);
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 0);
    expect(rowText(snap[0]) == "AAA", "CSI 0J keeps text above cursor");
    expect(rowText(snap[1]).empty(), "CSI 0J clears cursor line to end");

    sshterm::clear();
    sshterm::feed(text, sizeof(text) - 1);
    const char cup2[] = "\x1b[2;2H";
    sshterm::feed(cup2, sizeof(cup2) - 1);
    const char erase1[] = "\x1b[1J";
    sshterm::feed(erase1, sizeof(erase1) - 1);
    sshterm::snapshot(snap, 0);
    expect(rowText(snap[0]).empty(), "CSI 1J clears rows above cursor");
    expect(rowText(snap[1]) == "  B", "CSI 1J clears through cursor only");
}

void testHugeScroll()
{
    sshterm::clear();
    const char a[] = "X";
    sshterm::feed(a, sizeof(a) - 1);
    const char huge[] = "\x1b[999999999S";
    sshterm::feed(huge, sizeof(huge) - 1);
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 0);
    expect(true, "huge CSI S saturates without hanging");
}

void testHistory()
{
    sshterm::clear();
    for (int i = 0; i < 20; ++i) {
        char line[16];
        const int n = std::snprintf(line, sizeof(line), "L%d\r\n", i);
        sshterm::feed(line, n);
    }
    expect(sshterm::historyCount() > 0, "scroll pushes history");
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 1);
    expect(rowText(snap[0]).size() > 0, "history snapshot offset 1 non-empty");
}

void testAlternateScreen()
{
    sshterm::clear();
    const char main[] = "MAIN";
    sshterm::feed(main, sizeof(main) - 1);
    const char enter[] = "\x1b[?1049h";
    sshterm::feed(enter, sizeof(enter) - 1);
    const char alt[] = "ALT";
    sshterm::feed(alt, sizeof(alt) - 1);
    sshterm::Cell snap[sshterm::kRows][sshterm::kCols];
    sshterm::snapshot(snap, 0);
    expect(rowText(snap[0]) == "ALT", "1049h enters alternate screen");

    const char leave[] = "\x1b[?1049l";
    sshterm::feed(leave, sizeof(leave) - 1);
    sshterm::snapshot(snap, 0);
    expect(rowText(snap[0]) == "MAIN", "1049l restores main screen");
}

}  // namespace

int main()
{
    std::printf("SSH terminal tests\n");
    testPlainText();
    testSgrColor();
    testUtf8Box();
    testEraseModes();
    testHugeScroll();
    testHistory();
    testAlternateScreen();
    std::printf("%d passed, %d failed\n", 14 - failures, failures);
    return failures == 0 ? 0 : 1;
}
