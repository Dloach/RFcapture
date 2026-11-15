// Host tests for the store-rotation helper (src/atomic_store.h).
//
//   .\tests\host\run.ps1
//
// The point of the helper is behaviour under failure, so the fake file system
// below fails on demand and the tests check what a power cut would leave behind
// at every step.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "atomic_store.h"

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL %d: %s\n", line, what);
}

#define CHECK(cond) check((cond), #cond, __LINE__)

// Minimal stand-in for fs::FS: just the four calls rotate() makes.
struct FakeFs {
    std::map<std::string, std::string> files;

    int renameCount   = 0;
    int failRenameAt  = -1;  // 1-based index of the rename call that fails

    bool exists(const char* p) { return files.find(p) != files.end(); }

    bool remove(const char* p) { return files.erase(p) > 0; }

    bool rename(const char* from, const char* to)
    {
        ++renameCount;
        if (renameCount == failRenameAt) return false;
        auto it = files.find(from);
        if (it == files.end()) return false;
        const std::string data = it->second;
        files.erase(it);
        files[to] = data;
        return true;
    }

    void put(const char* p, const char* data) { files[p] = data; }
    bool has(const char* p) { return files.find(p) != files.end(); }
    std::string get(const char* p)
    {
        auto it = files.find(p);
        return it == files.end() ? std::string() : it->second;
    }
};

// A fresh save with no previous file.
void testFirstSave()
{
    FakeFs fs;
    fs.put("/a.tmp", "new");
    CHECK(atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.txt") == "new");
    CHECK(!fs.has("/a.tmp"));
    CHECK(!fs.has("/a.bak"));
}

// Replacing an existing file keeps the previous copy as .bak.
void testReplace()
{
    FakeFs fs;
    fs.put("/a.txt", "old");
    fs.put("/a.tmp", "new");
    CHECK(atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.txt") == "new");
    CHECK(fs.get("/a.bak") == "old");
    CHECK(!fs.has("/a.tmp"));
}

// An older backup is superseded, not accumulated.
void testStaleBackupReplaced()
{
    FakeFs fs;
    fs.put("/a.txt", "old");
    fs.put("/a.bak", "older");
    fs.put("/a.tmp", "new");
    CHECK(atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.txt") == "new");
    CHECK(fs.get("/a.bak") == "old");
}

// Power cut before the temp file exists: nothing to do, nothing changed.
void testNoTemp()
{
    FakeFs fs;
    fs.put("/a.txt", "old");
    CHECK(!atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.txt") == "old");
}

// Failure moving the old file aside: the new copy is dropped and the old file
// stays exactly where it was, so a failed save is a no-op.
void testFailRotateOut()
{
    FakeFs fs;
    fs.put("/a.txt", "old");
    fs.put("/a.tmp", "new");
    fs.failRenameAt = 1;  // /a.txt -> /a.bak fails
    CHECK(!atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.txt") == "old");
    CHECK(!fs.has("/a.tmp"));
}

// Failure moving the new file in: the old file is put back.
void testFailRotateIn()
{
    FakeFs fs;
    fs.put("/a.txt", "old");
    fs.put("/a.tmp", "new");
    fs.failRenameAt = 2;  // /a.tmp -> /a.txt fails after the backup succeeded
    CHECK(!atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.txt") == "old");
    CHECK(!fs.has("/a.tmp"));
}

// A null path is a programming error, not a crash.
void testNulls()
{
    FakeFs fs;
    fs.put("/a.tmp", "new");
    CHECK(!atomicstore::rotate(fs, nullptr, "/a.txt", "/a.bak"));
    CHECK(!atomicstore::rotate(fs, "/a.tmp", nullptr, "/a.bak"));
    CHECK(!atomicstore::rotate(fs, "/a.tmp", "/a.txt", nullptr));
    CHECK(fs.get("/a.tmp") == "new");
}

// The .bak the loader falls back to is the complete previous file, never a
// prefix of it.
void testBackupIsComplete()
{
    FakeFs fs;
    fs.put("/a.txt", "0123456789");
    fs.put("/a.tmp", "abcdefghij");
    CHECK(atomicstore::rotate(fs, "/a.tmp", "/a.txt", "/a.bak"));
    CHECK(fs.get("/a.bak") == "0123456789");
    CHECK(fs.get("/a.txt") == "abcdefghij");
}

}  // namespace

int main()
{
    testFirstSave();
    testReplace();
    testStaleBackupReplaced();
    testNoTemp();
    testFailRotateOut();
    testFailRotateIn();
    testNulls();
    testBackupIsComplete();

    if (g_failures == 0) {
        std::printf("atomic store: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("atomic store: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
}
