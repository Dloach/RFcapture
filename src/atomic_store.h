/*
 * Write-then-rotate for the small text stores (SPIFFS and the TF card alike).
 *
 * The pattern every store here uses:
 *
 *   write the complete new content into   <path>.tmp
 *   verify it (the caller re-opens and parses or size-checks it)
 *   rotate:  <path>     -> <path>.bak
 *            <path>.tmp -> <path>
 *
 * A power cut, a reset, or a card pulled mid-write can then leave:
 *   - the old file untouched (the .tmp is incomplete),
 *   - the old file plus a .bak (the rotation had not finished),
 *   - the new file (done).
 * It can never leave a half-written store in place of a good one.  The loaders
 * read <path> and fall back to <path>.bak when <path> is missing or does not
 * parse.
 *
 * The previous version is deliberately kept as .bak rather than deleted: disk
 * cost is a few kilobytes, and it is the only copy left if the new file turns
 * out to be unreadable.
 */
#pragma once

namespace atomicstore {

/// Rotate `tmp` into `path`, keeping the previous `path` as `bak`.
/// Returns false (and removes the temp file) when the rotation fails; the old
/// file is restored in that case, so a failed save never loses data.
template <typename FS>
bool rotate(FS& fs, const char* tmp, const char* path, const char* bak)
{
    if (tmp == nullptr || path == nullptr || bak == nullptr) return false;
    if (!fs.exists(tmp)) return false;

    if (fs.exists(path)) {
        if (fs.exists(bak)) fs.remove(bak);
        if (!fs.rename(path, bak)) {
            // Without a backup slot the rotation cannot be made safe.
            fs.remove(tmp);
            return false;
        }
    }

    if (!fs.rename(tmp, path)) {
        if (fs.exists(bak)) fs.rename(bak, path);  // put the old one back
        fs.remove(tmp);
        return false;
    }
    return true;
}

}  // namespace atomicstore
