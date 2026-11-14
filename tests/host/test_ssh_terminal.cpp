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

}  // namespace

int main()
{
    std::printf("SSH terminal tests\n");
    testPlainText();
    testSgrColor();
    testUtf8Box();
    testHistory();
    std::printf("%d passed, %d failed\n", 7 - failures, failures);
    return failures == 0 ? 0 : 1;
}
