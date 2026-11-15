/*
 * SPIFFS mounting, without the accidental format.
 *
 * `SPIFFS.begin(true)` formats the partition when the mount fails.  A mount can
 * fail for reasons that have nothing to do with corruption -- a power glitch, a
 * flash busy period, or simply being called before the driver is ready -- and
 * the result is that SSH profiles, the NFC card library and the quick-bind grid
 * are wiped by the act of *reading* them.
 *
 * So: mounting never formats.  Formatting is a separate call that only a user
 * action may reach (FILES -> SPIFFS, "F" twice), and it is explicit about being
 * destructive.
 *
 * Mount state is cached: SPIFFS.begin() on an already-mounted partition is
 * cheap, but the cache also remembers a successful format so later calls do not
 * retry a mount that cannot succeed.
 */
#pragma once

#include <SPIFFS.h>

namespace spiffsstore {

namespace detail {
inline bool& mountedFlag()
{
    static bool mounted = false;
    return mounted;
}
}  // namespace detail

/// Mount the partition if needed.  Never formats.  True when the file system is
/// usable.
inline bool ensure()
{
    if (detail::mountedFlag()) return true;
    if (SPIFFS.begin(false)) {
        detail::mountedFlag() = true;
        return true;
    }
    // A device that has never been formatted has no file system yet; that is a
    // legitimate "not mounted", not an error worth destroying data over.
    return false;
}

/// Destructive: erase the partition and mount it empty.  Only for an explicit
/// user request.
inline bool formatAndMount()
{
    Serial.println("[spiffs] FORMAT requested by user");
    SPIFFS.end();
    detail::mountedFlag() = false;
    if (!SPIFFS.format()) return false;
    if (!SPIFFS.begin(false)) return false;
    detail::mountedFlag() = true;
    return true;
}

/// True once ensure() or formatAndMount() succeeded in this boot.
inline bool mounted() { return detail::mountedFlag(); }

}  // namespace spiffsstore
