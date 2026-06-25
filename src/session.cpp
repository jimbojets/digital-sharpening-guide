#include "session.h"

#ifdef UNIT_TEST
namespace {
    SessionState g_state;
}
namespace session {
    void          begin() {}
    SessionState& state() { return g_state; }
    bool          has_session() { return g_state.active; }
    void          mark_active(const SessionState& s) { g_state = s; g_state.active = true; }
    void          clear() { g_state = SessionState{}; g_state.active = false; }
}
#else
#include <Arduino.h>

// Updated memory attributes for the ESP32-S3 chip.
// RTC_NOINIT_ATTR blocks the S3 bootloader from cleanly zeroing out this 
// memory zone on soft reboots, preserving your session states safely.
RTC_NOINIT_ATTR static SessionState g_state;
RTC_NOINIT_ATTR static bool         g_state_valid;

namespace session {
    void begin() {
        // Reset if RTC RAM was never initialized (cold boot) OR holds a struct
        // from a different firmware layout (magic/version mismatch after a flash).
        if (!g_state_valid
            || g_state.magic   != SESSION_MAGIC
            || g_state.version != SESSION_VERSION) {
            g_state = SessionState{};
            g_state.magic = SESSION_MAGIC;
            g_state.version = SESSION_VERSION;
            g_state_valid = true;
        }
    }
    SessionState& state() { return g_state; }
    bool has_session()    { return g_state.active; }
    void mark_active(const SessionState& s) {
        g_state = s;
        g_state.magic = SESSION_MAGIC;       // Enforce structural boundaries
        g_state.version = SESSION_VERSION;
        g_state.active = true;
    }
    void clear() {
        g_state = SessionState{};
        g_state.magic = SESSION_MAGIC;
        g_state.version = SESSION_VERSION;
        g_state.active = false;
    }
}
#endif
